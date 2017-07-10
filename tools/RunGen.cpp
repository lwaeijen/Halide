#include "HalideRuntime.h"
#include "HalideBuffer.h"
#include "halide_benchmark.h"
#include "halide_image_io.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

extern "C" int halide_rungen_redirect_argv(void **args);
extern "C" const struct halide_filter_metadata_t *halide_rungen_redirect_metadata();

// Buffer<> uses "shape" to mean "array of halide_dimension_t", but doesn't
// provide a typedef for it (and doesn't use a vector for it in any event).
using Shape = std::vector<halide_dimension_t>;

namespace {

using Halide::Runtime::Buffer;
using Halide::Tools::FormatInfo;

bool verbose = false;
bool halide_print_to_stdout = true;

// Standard stream output for halide_type_t
std::ostream &operator<<(std::ostream &stream, const halide_type_t &type) {
    if (type.code == halide_type_uint && type.bits == 1) {
        stream << "bool";
    } else {
        switch (type.code) {
        case halide_type_int:
            stream << "int";
            break;
        case halide_type_uint:
            stream << "uint";
            break;
        case halide_type_float:
            stream << "float";
            break;
        case halide_type_handle:
            stream << "handle";
            break;
        default:
            stream << "#unknown";
            break;
        }
        stream << std::to_string(type.bits);
    }
    if (type.lanes > 1) {
        stream << "x" + std::to_string(type.lanes);
    }
    return stream;
}

// Standard stream output for halide_dimension_t
std::ostream &operator<<(std::ostream &stream, const halide_dimension_t &d) {
    stream << "[" << d.min << "," << d.extent << "," << d.stride << "]";
    return stream;
}

// Standard stream output for vector<halide_dimension_t>
std::ostream &operator<<(std::ostream &stream, const Shape &shape) {
    stream << "[";
    bool need_comma = false;
    for (auto &d : shape) {
        if (need_comma) {
            stream << ',';
        }
        stream << d;
        need_comma = true;
    }
    stream << "]";
    return stream;
}

// Log informational output to stderr, but only in verbose mode
struct info {
    std::ostringstream msg;

    template<typename T>
    info &operator<<(const T &x) {
        if (verbose) {
            msg << x;
        }
        return *this;
    }

    ~info() {
        if (verbose) {
            std::cerr << msg.str();
            if (msg.str().back() != '\n') {
                std::cerr << '\n';
            }
        }
    }
};

// Log warnings to stderr
struct warn {
    std::ostringstream msg;

    template<typename T>
    warn &operator<<(const T &x) {
        msg << x;
        return *this;
    }

    ~warn() {
        std::cerr << "Warning: " << msg.str();
        if (msg.str().back() != '\n') {
            std::cerr << '\n';
        }
    }
};

// Log unrecoverable errors to stderr, then exit
struct fail {
    std::ostringstream msg;

    template<typename T>
    fail &operator<<(const T &x) {
        msg << x;
        return *this;
    }

    #ifdef _MSC_VER
    #pragma warning(push)
    #pragma warning(disable:4722)  // destructor never returns, potential memory leak
    #endif
    ~fail() {
        std::cerr << msg.str();
        if (msg.str().back() != '\n') {
            std::cerr << '\n';
        }
        exit(1);
    }
    #ifdef _MSC_VER
    #pragma warning(pop)
    #endif
};

// Replace the failure handlers from halide_image_io to fail()
bool IOCheckFail(bool condition, const char* msg) {
    if (!condition) {
        fail() << "Error in I/O: " << msg;
    }
    return condition;
}

// Replace the standard Halide runtime function to capture print output to stdout
void rungen_halide_print(void *user_context, const char *message) {
    if (halide_print_to_stdout) {
        std::cout << "halide_print: " << message;
    }
}

// Replace the standard Halide runtime function to capture Halide errors to fail()
void rungen_halide_error(void *user_context, const char *message) {
    fail() << "halide_error: " << message;
}

// Utility class for installing memory-tracking machinery into the Halide runtime
// when --track_memory is specified.
class HalideMemoryTracker {
    static HalideMemoryTracker *active;

    std::mutex tracker_mutex;

    // Total current CPU memory allocated via halide_malloc.
    // Access controlled by tracker_mutex.
    uint64_t memory_allocated;

    // High-water mark of CPU memory allocated since program start
    // (or last call to get_cpu_memory_highwater_reset).
    // Access controlled by tracker_mutex.
    uint64_t memory_highwater;

    // Map of outstanding allocation sizes.
    // Access controlled by tracker_mutex.
    std::map<void *, size_t> memory_size_map;

    void *tracker_malloc_impl(void *user_context, size_t x) {
        std::lock_guard<std::mutex> lock(tracker_mutex);

        void *ptr = halide_default_malloc(user_context, x);

        memory_allocated += x;
        if (memory_highwater < memory_allocated) {
            memory_highwater = memory_allocated;
        }
        if (memory_size_map.find(ptr) != memory_size_map.end()) {
            halide_error(user_context, "Tracking error in tracker_malloc");
        }
        memory_size_map[ptr] = x;

        return ptr;
    }

    void tracker_free_impl(void *user_context, void *ptr) {
        std::lock_guard<std::mutex> lock(tracker_mutex);
        auto it = memory_size_map.find(ptr);
        if (it == memory_size_map.end()) {
            halide_error(user_context, "Tracking error in tracker_free");
        }
        size_t x = it->second;
        memory_allocated -= x;
        memory_size_map.erase(it);
        halide_default_free(user_context, ptr);
    }

    static void *tracker_malloc(void *user_context, size_t x) {
        return active->tracker_malloc_impl(user_context, x);
    }

    static void tracker_free(void *user_context, void *ptr) {
        return active->tracker_free_impl(user_context, ptr);
    }

  public:
    void install() {
        assert(!active);
        active = this;
        halide_set_custom_malloc(tracker_malloc);
        halide_set_custom_free(tracker_free);
    }

    uint64_t allocated() {
        std::lock_guard<std::mutex> lock(tracker_mutex);
        return memory_allocated;
    }

    uint64_t highwater() {
        std::lock_guard<std::mutex> lock(tracker_mutex);
        return memory_highwater;
    }

    void highwater_reset() {
        std::lock_guard<std::mutex> lock(tracker_mutex);
        memory_highwater = memory_allocated;
    }
};

/* static */ HalideMemoryTracker *HalideMemoryTracker::active{nullptr};

std::vector<std::string> split_string(const std::string &source, 
                                      const std::string &delim) {
    std::vector<std::string> elements;
    size_t start = 0;
    size_t found = 0;
    while ((found = source.find(delim, start)) != std::string::npos) {
        elements.push_back(source.substr(start, found - start));
        start = found + delim.size();
    }

    // If start is exactly source.size(), the last thing in source is a
    // delimiter, in which case we want to add an empty std::string to elements.
    if (start <= source.size()) {
        elements.push_back(source.substr(start, std::string::npos));
    }
    return elements;
}

std::string replace_all(const std::string &str, 
                        const std::string &find, 
                        const std::string &replace) {
    size_t pos = 0;
    std::string result = str;
    while ((pos = result.find(find, pos)) != std::string::npos) {
        result.replace(pos, find.length(), replace);
        pos += replace.length();
    }
    return result;
}

// Must be constexpr to allow use in case clauses.
inline constexpr int halide_type_code(halide_type_code_t code, int bits) {
    return (((int) code) << 8) | bits;
}

// dynamic_type_dispatch is a utility for functors that want to be able
// to dynamically dispatch a halide_type_t to type-specialized code.
// To use it, a functor must be a *templated* class, e.g.
//
//     template<typename T> class MyFunctor { int operator()(arg1, arg2...); };
//
// dynamic_type_dispatch() is called with a halide_type_t as the first argument,
// followed by the arguments to the Functor's operator():
//
//     auto result = dynamic_type_dispatch<MyFunctor>(some_halide_type, arg1, arg2);
//
// Note that this means that the functor must be able to instantiate its
// operator() for all the Halide scalar types; it also means that all those
// variants *will* be instantiated (increasing code size), so this approach
// should only be used when strictly necessary.
template<template<typename> class Functor, typename... Args>
auto dynamic_type_dispatch(const halide_type_t &type, Args&&... args) -> 
    decltype(std::declval<Functor<uint8_t>>()(std::forward<Args>(args)...)) {

#define HANDLE_CASE(CODE, BITS, TYPE) \
    case halide_type_code(CODE, BITS): return Functor<TYPE>()(std::forward<Args>(args)...);
    switch (halide_type_code((halide_type_code_t) type.code, type.bits)) {
        HANDLE_CASE(halide_type_float, 32, float)
        HANDLE_CASE(halide_type_float, 64, double)
        HANDLE_CASE(halide_type_int, 8, int8_t)
        HANDLE_CASE(halide_type_int, 16, int16_t)
        HANDLE_CASE(halide_type_int, 32, int32_t)
        HANDLE_CASE(halide_type_int, 64, int64_t)
        HANDLE_CASE(halide_type_uint, 1, bool)
        HANDLE_CASE(halide_type_uint, 8, uint8_t)
        HANDLE_CASE(halide_type_uint, 16, uint16_t)
        HANDLE_CASE(halide_type_uint, 32, uint32_t)
        HANDLE_CASE(halide_type_uint, 64, uint64_t)
        HANDLE_CASE(halide_type_handle, 64, void*)
        default:
            fail() << "Unsupported type: " << type << "\n";
            using ReturnType = decltype(std::declval<Functor<uint8_t>>()(std::forward<Args>(args)...));
            return ReturnType();
    }
#undef HANDLE_CASE
}

// Functor to parse a string into one of the known Halide scalar types.
template<typename T>
struct ScalarParser {
    bool operator()(const std::string &str, halide_scalar_value_t *v) {
        std::istringstream iss(str);
        iss >> *(T*)v;
        return !iss.fail() && iss.get() == EOF;
    }
};

// Override for bool, since istream just expects '1' or '0'.
template<>
bool ScalarParser<bool>::operator()(const std::string &str, halide_scalar_value_t *v) {
    if (str == "true") {
        v->u.b = true;
        return true;
    }
    if (str == "false") {
        v->u.b = false;
        return true;
    }
    return false;
}

// Override for handle, since we only accept "nullptr".
template<>
bool ScalarParser<void*>::operator()(const std::string &str, halide_scalar_value_t *v) {
    if (str == "nullptr") {
        v->u.handle = nullptr;
        return true;
    }
    return false;
}

// Parse a scalar when we know the corresponding C++ type at compile time.
template<typename T>
bool parse_scalar(const std::string &str, T *scalar) {
    return ScalarParser<T>()(str, (halide_scalar_value_t *) scalar);
}

// Dynamic-dispatch wrapper around ScalarParser.
bool parse_scalar(const halide_type_t &type,
                  const std::string &str,
                  halide_scalar_value_t *scalar) {
    return dynamic_type_dispatch<ScalarParser>(type, str, scalar);
}

// Parse an extent list, which should be of the form
//
//    [extent0, extent1...]
//
// Return a vector<halide_dimension_t> (aka a "shape") with the extents filled in,
// but with the min and stride of each dimension set to zero.
Shape parse_extents(const std::string &extent_list) {
    if (extent_list.empty() || extent_list[0] != '[' || extent_list.back() != ']') {
        fail() << "Invalid format for extents: " << extent_list;
    }
    Shape result;
    std::vector<std::string> extents = split_string(extent_list.substr(1, extent_list.size()-2), ",");
    for (auto &s : extents) {
        halide_dimension_t d = {0, 0, 0};
        if (!parse_scalar(s, &d.extent)) {
            fail() << "Invalid value for extents: " << s << " (" << extent_list << ")";
        }
        result.push_back(d);
    }
    return result;
}

// BEGIN TODO: hacky algorithm inspired by Safelight
// (should really use the algorithm from AddImageChecks to come up with something more rigorous.)
Shape choose_output_extents(int dimensions, const Shape &defaults) {
    Shape s(dimensions);
    for (int i = 0; i < dimensions; ++i) {
        if ((size_t) i < defaults.size()) {
            s[i] = defaults[i];
            continue;
        }
        s[i].extent = (i < 2 ? 1000 : 4);
    }
    return s;
}

Shape fix_bounds_query_shape(const Shape &constrained_shape) {
    Shape new_shape = constrained_shape;

    // Make sure that the extents and strides for these are nonzero.
    for (size_t i = 0; i < new_shape.size(); ++i) {
        if (!new_shape[i].extent) {
            // A bit of a hack: fill in unconstrained dimensions to 1... except
            // for probably-the-channels dimension, which we'll special-case to
            // fill in to 4 when possible (unless it appears to be chunky).
            // Stride will be fixed below.
            if (i == 2) {
                if (constrained_shape[0].stride >= 1 && constrained_shape[2].stride == 1) {
                    // Definitely chunky, so make extent[2] match the chunk size
                    new_shape[i].extent = constrained_shape[0].stride;
                } else {
                    // Not obviously chunky; let's go with 4 channels.
                    new_shape[i].extent = 4;
                }
            } else {
                new_shape[i].extent = 1;
            }
        }
    }

    // Special-case Chunky: most "chunky" generators tend to constrain stride[0]
    // and stride[2] to exact values, leaving stride[1] unconstrained;
    // in practice, we must ensure that stride[1] == stride[0] * extent[0]
    // and stride[0] = extent[2] to get results that are not garbled.
    // This is unpleasantly hacky and will likely need aditional enhancements.
    // (Note that there are, theoretically, other stride combinations that might
    // need fixing; in practice, ~all generators that aren't planar tend
    // to be classically chunky.)
    if (new_shape.size() >= 3) {
        if (constrained_shape[2].stride == 1) {
            if (constrained_shape[0].stride >= 1) {
                // If we have stride[0] and stride[2] std::set to obviously-chunky,
                // then force extent[2] to match stride[0].
                new_shape[2].extent = constrained_shape[0].stride;
            } else {
                // If we have stride[2] == 1 but stride[0] <= 1,
                // force stride[0] = extent[2]
                new_shape[0].stride = new_shape[2].extent;
            }
            // Ensure stride[1] is reasonable.
            new_shape[1].stride = new_shape[0].extent * new_shape[0].stride;
        }
    }

    // If anything else is zero, just set strides to planar and hope for the best.
    bool zero_strides = false;
    for (size_t i = 0; i < new_shape.size(); ++i) {
        if (!new_shape[i].stride) {
            zero_strides = true;
        }
    }
    if (zero_strides) {
        // Planar
        new_shape[0].stride = 1;
        for (size_t i = 1; i < new_shape.size(); ++i) {
            new_shape[i].stride = new_shape[i - 1].stride * new_shape[i - 1].extent;
        }
    }
    return new_shape;
}
// END TODO: hacky algorithm inspired by Safelight

// Given a Buffer<>, return its shape in the form of a vector<halide_dimension_t>.
// (Oddly, Buffer<> has no API to do this directly.)
Shape get_shape(const Buffer<> &b) {
    Shape s;
    for (int i = 0; i < b.dimensions(); ++i) {
        s.push_back(b.raw_buffer()->dim[i]);
    }
    return s;
}

// Given a type and shape, create a new Buffer<> and allocate storage for it.
// (Oddly, Buffer<> has an API to do this with vector-of-extent, but not vector-of-halide_dimension_t.)
Buffer<> allocate_buffer(const halide_type_t &type, const Shape &shape) {
    Buffer<> b(type, nullptr, (int) shape.size(), &shape[0]);
    b.check_overflow();
    b.allocate();
    return b;
}

// Return true iff all of the dimensions in the range [first, last] have an extent of <= 1.
bool dims_in_range_are_trivial(const Buffer<> &b, int first, int last) {
    for (int d = first; d <= last; ++d) {
        if (b.dim(d).extent() > 1) {
            return false;
        }
    }
    return true;
}

// Add or subtract dimensions to the given buffer to match dims_needed, 
// emitting warnings if we do so.
Buffer<> adjust_buffer_dims(const std::string &title, const std::string &name, const int dims_needed, Buffer<> b) {
    const int dims_actual = b.dimensions();
    if (dims_actual > dims_needed) {
        // Warn that we are ignoring dimensions, but only if at least one of the ignored dimensions has extent > 1
        if (!dims_in_range_are_trivial(b, dims_needed, dims_actual - 1)) {
            warn() << "Image for " << title << " \"" << name << "\" has " 
                 << dims_actual << " dimensions, but only the first "
                 << dims_needed << " were used; data loss may have occurred.";
        }
        auto old_shape = get_shape(b);
        while (b.dimensions() > dims_needed) {
            b = b.sliced(dims_needed, 0);
        }
        info() << "Shape for " << name << " changed: " << old_shape << " -> " << get_shape(b);
    } else if (dims_actual < dims_needed) {
        warn() << "Image for " << title << " \"" << name << "\" has " 
             << dims_actual << " dimensions, but this argument requires at least "
             << dims_needed << " dimensions: adding dummy dimensions of extent 1.";
        auto old_shape = get_shape(b);
        while (b.dimensions() < dims_needed) {
            b = b.embedded(b.dimensions(), 0);
        }
        info() << "Shape for " << name << " changed: " << old_shape << " -> " << get_shape(b);
    }
    return b;
}

// Load a buffer from a pathname, adjusting the type and dimensions to
// fit the metadata's requirements as needed.
Buffer<> load_input_from_file(const std::string &pathname, 
                              const halide_filter_argument_t &metadata) {
    Buffer<> b = Buffer<>(metadata.type, 0);
    info() << "Loading input " << metadata.name << " from " << pathname << " ...";
    if (!Halide::Tools::load<Buffer<>, IOCheckFail>(pathname, &b)) {
        fail() << "Unable to load input: " << pathname;
    }
    if (b.dimensions() != metadata.dimensions) {
        b = adjust_buffer_dims("Input", metadata.name, metadata.dimensions, b);
    }
    if (b.type() != metadata.type) {
        warn() << "Image loaded for argument \"" << metadata.name << "\" is type " 
             << b.type() << " but this argument expects type "
             << metadata.type << "; data loss may have occurred.";
        b = Halide::Tools::ImageTypeConversion::convert_image(b, metadata.type);
    }
    return b;
}

template<typename T>
struct Zeroer {
    bool operator()(Buffer<> *image) {
        Buffer<T> &b = image->as<T>();
        b.fill((T) 0);
        return true;
    }
};

Buffer<> load_input(const std::string &pathname, 
                    const halide_filter_argument_t &metadata) {
    std::vector<std::string> v = split_string(pathname, ":");
    if (v.size() != 2 || v[0].size() == 1) {
        return load_input_from_file(pathname, metadata);
    }

    // Assume it's a special std::string of the form key:values
    if (v[0] == "zero") {
        auto shape = parse_extents(v[1]);
        Buffer<> b = allocate_buffer(metadata.type, shape);
        (void) dynamic_type_dispatch<Zeroer>(b.type(), &b);
        return b;
    }

    // TODO: add random options.
    // TODO: add granger-rainbow.
    // TODO: add gradients.

    fail() << "Unknown input: " << pathname;
    return Buffer<>();
}

void usage(const char *argv0) {
const std::string usage = R"USAGE(
Usage: $NAME$ argument=value [argument=value... ] [flags]

Arguments:

    Specify the Generator's input and output values by name, in any order.

    Scalar inputs are specified in the obvious syntax, e.g.

        some_int=42 some_float=3.1415

    Buffer inputs and outputs are specified by pathname:

        some_input_buffer=/path/to/existing/file.png
        some_output_buffer=/path/to/create/output/file.png

    We currently support JPG, PGM, PNG, PPM format. If the type or dimensions 
    of the input or output file type can't support the data (e.g., your filter 
    uses float32 input and output, and you load/save to PNG), we'll use the most 
    robust approximation within the format and issue a warning to stdout.

    (We anticipate adding other image formats in the future, in particular,
    TIFF and TMP.)

    For inputs, there are also "pseudo-file" specifiers you can use; currently
    supported are

        zero:[NUM,NUM,...]

        This input should be an image with the given extents, and all elements
        set to zero of the appropriate type. (This is useful for benchmarking
        filters that don't have performance variances with different data.)

        (We anticipate adding other pseudo-file inputs in the future, e.g.
        various random distributions, gradients, rainbows, etc.)

Flags:

    --describe:     
        print names and types of all arguments to stdout and exit.

    --output_extents=[NUM,NUM,...]
        Normally we attempt to guess a reasonable size for the output buffers,
        based on the size of the input buffers and bounds query; if we guess
        wrong, or you want to explicitly specify the desired output size,
        you can specify the extent of each dimension with this flag:

        --output_extents=[1000,100]   # 2 dimensions: w=1000 h = 100
        --output_extents=[100,200,3]  # 3 dimensions: w=100 h=200 c=3

        Note that if there are multiple outputs, all will be constrained
        to this shape.

    --verbose:      
        emit extra diagnostic output.

    --print:
        Log calls to halide_print() to stdout. (This is the default; use
        --print=false to silence noisy Generators.)

    --benchmark:    
        Run the filter with the given arguments many times to 
        produce an estimate of average execution time; this currently
        runs "samples" sets of "iterations" each, and chooses the fastest
        sample set.

    --benchmark_samples=NUM:
        Override the default number of benchmarking sample sets; ignored if 
        --benchmark is not also specified.

    --benchmark_iterations=NUM: 
        Override the default number of benchmarking iterations; ignored if 
        --benchmark is not also specified.

    --track_memory: 
        Override Halide memory allocator to track high-water mark of memory 
        allocation during run; note that this may slow down execution, so 
        benchmarks may be inaccurate if you combine --benchmark with this.

Known Issues:

    * Filters running on GPU (vs CPU) have not been tested.
    * Filters using buffer layouts other than planar (e.g. interleaved/chunky)
      may be buggy.

)USAGE";

    std::string basename = split_string(replace_all(argv0, "\\", "/"), "/").back();
    std::cout << replace_all(usage, "$NAME$", basename);
}

void do_describe(const halide_filter_metadata_t *md) {
    std::cout << "Filter name: \"" << md->name << "\"\n";
    for (size_t i = 0; i < (size_t) md->num_arguments; ++i) {
        auto &a = md->arguments[i];
        bool is_input = a.kind != halide_argument_kind_output_buffer;
        bool is_scalar = a.kind == halide_argument_kind_input_scalar;
        std::cout << "  " << (is_input ? "Input" : "Output") << " \"" << a.name << "\" is of type ";
        if (is_scalar) {
            std::cout << a.type;
        } else {
            std::cout << "Buffer<" << a.type << "> with " << a.dimensions << " dimensions";
        }
        std::cout << "\n";
    }
}

// This logic exists in Halide::Tools, but is Internal; we're going to replicate
// it here for now since we may want slightly different logic in some cases
// for this tool.
FormatInfo best_save_format(const Buffer<> &b, const std::set<FormatInfo> &info) {
    // Perfect score is zero (exact match).
    // The larger the score, the worse the match.
    int best_score = 0x7fffffff;
    FormatInfo best{};
    const halide_type_t type = b.type();
    const int dimensions = b.dimensions();
    for (auto &f : info) {
        int score = 0;
        // If format has too-few dimensions, that's very bad.
        score += std::abs(f.dimensions - dimensions) * 128;
        // If format has too-few bits, that's pretty bad.
        score += std::abs(f.type.bits - type.bits);
        // If format has different code, that's a little bad.
        score += (f.type.code != type.code) ? 1 : 0;
        if (score < best_score) {
            best_score = score;
            best = f;
        }
    }

    return best;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc <= 1) {
        usage(argv[0]);
        return 0;
    }

    halide_set_error_handler(rungen_halide_error);
    halide_set_custom_print(rungen_halide_print);

    const halide_filter_metadata_t *md = halide_rungen_redirect_metadata();

    struct ArgData {
        size_t index{0};
        const halide_filter_argument_t *metadata{nullptr};
        std::string raw_string;
        halide_scalar_value_t scalar_value;
        Buffer<> buffer_value;
    };

    std::map<std::string, ArgData> args;
    std::set<std::string> found;
    for (size_t i = 0; i < (size_t) md->num_arguments; ++i) {
        std::string name = md->arguments[i].name;
        ArgData arg;
        arg.index = i;
        arg.metadata = &md->arguments[i];
        if (arg.metadata->type.code == halide_type_handle) {
            // Pre-populate handle types with a default value of 'nullptr'
            // (the only legal value), so that they're ok to omit.
            arg.raw_string = "nullptr";
            found.insert(name);
        }
        args[name] = arg;
    }

    Shape default_output_shape;
    std::vector<void*> filter_argv(md->num_arguments, nullptr);
    std::vector<std::string> unknown_args;
    bool benchmark = false;
    bool track_memory = false;
    bool describe = false;
    int benchmark_samples = 3;
    int benchmark_iterations = 10;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') {
            const char *p = argv[i] + 1; // skip -
            if (p[0] == '-') {
                p++; // allow -- as well, because why not
            }
            std::vector<std::string> v = split_string(p, "=");
            std::string flag_name = v[0];
            std::string flag_value = v.size() > 1 ? v[1] : "";
            if (v.size() > 2) {
                fail() << "Invalid argument: " << argv[i];
            }
            if (flag_name == "verbose") {
                if (flag_value.empty()) {
                    flag_value = "true";
                }
                if (!parse_scalar(flag_value, &verbose)) {
                    fail() << "Invalid value for flag: " << flag_name;
                }
            } else if (flag_name == "print") {
                if (flag_value.empty()) {
                    flag_value = "true";
                }
                if (!parse_scalar(flag_value, &halide_print_to_stdout)) {
                    fail() << "Invalid value for flag: " << flag_name;
                }
            } else if (flag_name == "describe") {
                if (flag_value.empty()) {
                    flag_value = "true";
                }
                if (!parse_scalar(flag_value, &describe)) {
                    fail() << "Invalid value for flag: " << flag_name;
                }
            } else if (flag_name == "benchmark") {
                if (flag_value.empty()) {
                    flag_value = "true";
                }
                if (!parse_scalar(flag_value, &benchmark)) {
                    fail() << "Invalid value for flag: " << flag_name;
                }
            } else if (flag_name == "track_memory") {
                if (flag_value.empty()) {
                    flag_value = "true";
                }
                if (!parse_scalar(flag_value, &track_memory)) {
                    fail() << "Invalid value for flag: " << flag_name;
                }
            } else if (flag_name == "benchmark_samples") {
                if (!parse_scalar(flag_value, &benchmark_samples)) {
                    fail() << "Invalid value for flag: " << flag_name;
                }
            } else if (flag_name == "benchmark_iterations") {
                if (!parse_scalar(flag_value, &benchmark_iterations)) {
                    fail() << "Invalid value for flag: " << flag_name;
                }
            } else if (flag_name == "output_extents") {
                default_output_shape = parse_extents(flag_value);
            } else {
                usage(argv[0]);
                fail() << "Unknown flag: " << flag_name;
            }
        } else {
            // Assume it's a named Input or Output for the Generator,
            // in the form name=value.
            std::vector<std::string> v = split_string(argv[i], "=");
            if (v.size() != 2 || v[0].empty() || v[1].empty()) {
                fail() << "Invalid argument: " << argv[i];
            }
            const std::string &arg_name = v[0];
            const std::string &arg_value = v[1];
            if (args.find(arg_name) == args.end()) {
                // Gather up unknown-argument-names and show them
                // along with missing-argument-names, to make typos
                // easier to correct.
                unknown_args.push_back(arg_name);
                break;
            }
            if (arg_value.empty()) {
                fail() << "Argument value is empty for: " << arg_name;
            }
            auto &arg = args[arg_name];
            if (!arg.raw_string.empty()) {
                fail() << "Argument value specified multiple times for: " << arg_name;
            }
            arg.raw_string = arg_value;
            found.insert(arg_name);
        }
    }

    if (describe) {
        do_describe(md);
        return 0;
    }

    // It's OK to omit output arguments when we are benchmarking or tracking memory.
    bool ok_to_omit_outputs = (benchmark || track_memory);

    if (benchmark && track_memory) {
        warn() << "Using --track_memory with --benchmark will produce inaccurate benchmark results.";
    }

    // Check to be sure that all required arguments are specified.
    if (found.size() != args.size() || !unknown_args.empty()) {
        std::ostringstream o;
        for (auto &s : unknown_args) {
            o << "Unknown argument name: " << s << "\n";
        }
        for (auto &arg_pair : args) {
            auto &arg = arg_pair.second;
            if (arg.raw_string.empty()) {
                if (ok_to_omit_outputs && arg.metadata->kind == halide_argument_kind_output_buffer) {
                    continue;
                }
                o << "Argument value missing for: " << arg.metadata->name << "\n";
            }
        }
        if (!o.str().empty()) {
            fail() << o.str();
        }
    }

    // Parse all the input arguments, loading images as necessary.
    // (Don't handle outputs yet.)
    for (auto &arg_pair : args) {
        auto &arg_name = arg_pair.first;
        auto &arg = arg_pair.second;
        switch (arg.metadata->kind) {
        case halide_argument_kind_input_scalar: {
            if (!parse_scalar(arg.metadata->type, arg.raw_string, &arg.scalar_value)) {
                fail() << "Argument value for: " << arg_name << " could not be parsed as type " 
                     << arg.metadata->type << ": " 
                     << arg.raw_string;
            }
            filter_argv[arg.index] = &arg.scalar_value;
            break;
        }
        case halide_argument_kind_input_buffer: {
            arg.buffer_value = load_input(arg.raw_string, *arg.metadata);
            // If there was no default_output_shape specified, use the shape of
            // the first input buffer (if any). 
            // TODO: this is often a better-than-nothing guess, but not always. Add a way to defeat it?
            if (default_output_shape.empty()) {
                default_output_shape = get_shape(arg.buffer_value);
            }
            filter_argv[arg.index] = arg.buffer_value.raw_buffer();
            break;
        }
        case halide_argument_kind_output_buffer:
            // Nothing yet
            break;
        }
    }

    // Run a bounds query, so we can allocate output buffers appropriately.
    {
        for (auto &arg_pair : args) {
            auto &arg = arg_pair.second;
            switch (arg.metadata->kind) {
            case halide_argument_kind_output_buffer:
                auto bounds_query_shape = choose_output_extents(arg.metadata->dimensions, default_output_shape);
                arg.buffer_value = Buffer<>(arg.metadata->type, nullptr, (int) bounds_query_shape.size(), &bounds_query_shape[0]);
                filter_argv[arg.index] = arg.buffer_value.raw_buffer();
                break;
            }
        }

        info() << "Running bounds query...";
        int result = halide_rungen_redirect_argv(&filter_argv[0]);
        if (result != 0) {
            fail() << "Bounds query failed with result code: " << result;
        }
    }

    // Allocate the output buffers we'll need.
    double pixels_out = 0.f;
    for (auto &arg_pair : args) {
        auto &arg_name = arg_pair.first;
        auto &arg = arg_pair.second;
        switch (arg.metadata->kind) {
        case halide_argument_kind_output_buffer:
            auto constrained_shape = get_shape(arg.buffer_value);
            info() << "Output " << arg_name << ": BoundsQuery result is " << constrained_shape;
            Shape shape = fix_bounds_query_shape(constrained_shape);
            arg.buffer_value = allocate_buffer(arg.metadata->type, shape);
            info() << "Output " << arg_name << ": Shape is " << get_shape(arg.buffer_value);
            filter_argv[arg.index] = arg.buffer_value.raw_buffer();
            // TODO: this assumes that most output is "pixel-ish", and counting the size of the first
            // two dimensions approximates the "pixel size". This is not, in general, a valid assumption,
            // but is a useful metric for benchmarking.
            if (shape.size() >= 2) {
                pixels_out += shape[0].extent * shape[1].extent;
            } else {
                pixels_out += shape[0].extent;
            }
            break;
        }
    }
    double megapixels = pixels_out / (1024.0 * 1024.0);

    // If we're tracking memory, install the memory tracker *after* doing a bounds query.
    HalideMemoryTracker tracker;
    if (track_memory) {
        tracker.install();
    }

    if (benchmark) {
        info() << "Benchmarking filter...";

        // Run once to warm up cache. Ignore result since our halide_error() should catch everything.
        (void) halide_rungen_redirect_argv(&filter_argv[0]);

        double time_in_seconds = Halide::Tools::benchmark(benchmark_samples, benchmark_iterations, [&filter_argv]() { 
            (void) halide_rungen_redirect_argv(&filter_argv[0]);
        });

        std::cout << "Benchmark for " << md->name << " produces best case of " << time_in_seconds << " sec/iter, over " 
            << benchmark_samples << " blocks of " << benchmark_iterations << " iterations.\n";
        std::cout << "Best output throughput is " << (megapixels / time_in_seconds) << " mpix/sec.\n";

    } else {
        info() << "Running filter...";
        int result = halide_rungen_redirect_argv(&filter_argv[0]);
        if (result != 0) {
            fail() << "Filter failed with result code: " << result;
        }
    }

    if (track_memory) {
        std::cout << "Maximum Halide memory: " << tracker.highwater() 
            << " bytes for output of " << megapixels << " mpix.\n";
    }

    // Save the output(s), if necessary.
    for (auto &arg_pair : args) {
        auto &arg_name = arg_pair.first;
        auto &arg = arg_pair.second;
        if (arg.metadata->kind == halide_argument_kind_output_buffer) {
            if (!arg.raw_string.empty()) {
                info() << "Saving output " << arg_name << " to " << arg.raw_string << " ...";
                Buffer<> &b = arg.buffer_value;

                std::set<FormatInfo> savable_types;
                if (!Halide::Tools::save_query<Buffer<>, IOCheckFail>(arg.raw_string, &savable_types)) {
                    fail() << "Unable to save output: " << arg.raw_string;
                }
                const FormatInfo best = best_save_format(b, savable_types);
                if (best.dimensions != b.dimensions()) {
                    b = adjust_buffer_dims("Output", arg_name, best.dimensions, b);
                }
                if (best.type != b.type()) {
                    warn() << "Image for argument \"" << arg_name << "\" is of type " 
                         << b.type() << " but is being saved as type "
                         << best.type << "; data loss may have occurred.";
                    b = Halide::Tools::ImageTypeConversion::convert_image(b, best.type);
                }
                if (!Halide::Tools::save<Buffer<>, IOCheckFail>(b, arg.raw_string)) {
                    fail() << "Unable to save output: " << arg.raw_string;
                }
            } else {
                info() << "(Output " << arg_name << " was not saved.)";
            }
        }
    }

    return 0;
}
