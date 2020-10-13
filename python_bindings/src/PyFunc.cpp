#include "PyFunc.h"
#include "PyBinaryOperators.h"
#include "PyBuffer.h"
#include "PyExpr.h"
#include "PyFuncRef.h"
#include "PyLoopLevel.h"
#include "PyScheduleMethods.h"
#include "PyStage.h"
#include "PyTuple.h"
#include "PyVarOrRVar.h"
#include <string>
#include <unordered_map>
#include <pybind11/functional.h>

namespace Halide {
namespace PythonBindings {

namespace {

template<typename LHS>
void define_get(py::class_<Func> &func_class) {
    func_class
        .def("__getitem__", [](Func &func, const LHS &args) -> FuncRef {
            return func(args);
        })
    ;
}

template<typename LHS, typename RHS>
void define_set(py::class_<Func> &func_class) {
    func_class
        .def("__setitem__", [](Func &func, const LHS &lhs, const RHS &rhs) -> Stage {
            return func(lhs) = rhs;
        })
        .def("__setitem__", [](Func &func, const std::vector<LHS> &lhs, const RHS &rhs) -> Stage {
            return func(lhs) = rhs;
        })
    ;
}

py::object realization_to_object(const Realization &r) {
    // Only one Buffer -> just return it
    if (r.size() == 1) {
        return py::cast(r[0]);
    }

    // Multiple -> return as Python tuple
    return to_python_tuple(r);
}

}  // namespace

// Remove trailing $<int>
std::string sanitize_name(std::string name){
    std::size_t found = name.find('$');
    if( found != std::string::npos)
      return name.substr(0,found);
    return name;
}

// Typedef load and store counter struct
typedef struct counters_t{
    int stores;
    int loads;
}counters_t;

// Create a global map of string -> counter struct
std::unordered_map<std::string, counters_t> func2counters;

inline void init_counter(const std::string name){
     func2counters[sanitize_name(name)]={0,0};
}

inline void init_counter_ext(Func& f, const std::string name){
    init_counter(sanitize_name(name));
}

inline void count_accesses_internal(int etype, const std::string name){

  // Choose what to do based on event type
  if(etype==0){
    //load
     func2counters[sanitize_name(name)].loads++;
  }
  else if(etype==1){
    //store
    func2counters[sanitize_name(name)].stores++;
  }

  //else, nothing:
    //halide_trace_load = 0,
    //halide_trace_store = 1,
    //halide_trace_begin_realization = 2,
    //halide_trace_end_realization = 3,
    //halide_trace_produce = 4,
    //halide_trace_end_produce = 5,
    //halide_trace_consume = 6,
    //halide_trace_end_consume = 7,
    //halide_trace_begin_pipeline = 8,
    //halide_trace_end_pipeline = 9,
    //halide_trace_tag = 10
}

int count_accesses_safe(void *user_context, const halide_trace_event_t *e) {
  //not sure why this is required
  static int id=0;

  //init key if required
  if(func2counters.find(e->func)==func2counters.end())
    init_counter(e->func);

  count_accesses_internal(e->event,  e->func);

  return id++;
}

int count_accesses_unsafe(void *user_context, const halide_trace_event_t *e) {
  static int id=0;

  count_accesses_internal(e->event,  e->func);

  return id++;
}

void register_count_accesses(Func &f) {
    // Register custom trace function for this function
    f.set_custom_trace(&count_accesses_safe);
}

void register_count_accesses_unsafe(Func &f) {
    // Register custom trace function for this function
    f.set_custom_trace(&count_accesses_unsafe);
}

int get_loads(Func &f, const std::string name){
  if(func2counters.find(name)==func2counters.end())
    return -1;
  return func2counters[name].loads;
}

int get_stores(Func &f, const std::string name){
  if(func2counters.find(name)==func2counters.end())
    return -1;
  return func2counters[name].stores;
}

void print_counters(Func &f){
  for(auto& it: func2counters){
    std::cout << it.first << " loads: " << it.second.loads << std::endl;
    std::cout << it.first << " stores: " << it.second.stores << std::endl;
  }
}

void reset_counters(Func &f){
    func2counters.clear();
}

// Create a global map of buffer name -> size
std::unordered_map<std::string, int> mem_sizes;
void collect_mem_stats(void *, const char *msg){
    float ms;
    int percentage;
    int stack;
    int peak;
    int num;
    int avg;
    char name[64];

    if(sscanf(
        msg,
        " %[^:]: %fms (%d%%) stack: %d",
        name,
        &ms,
        &percentage,
        &stack
    )==4){
        mem_sizes[sanitize_name(name)] = stack;
        return;
    }

    if(sscanf(
      msg,
      " %[^:]: %fms (%d%%) peak: %d num: %d avg: %d",
      name,
      &ms,
      &percentage,
      &peak,
      &num,
      &avg
    )==6){
        mem_sizes[sanitize_name(name)] = peak;
        return;
    }

    if(sscanf(
      msg,
      " %[^:]: %fms (%d%%) peak: %d num: %d avg: %d stack: %d",
      name,
      &ms,
      &percentage,
      &peak,
      &num,
      &avg,
      &stack
    )==7){
        mem_sizes[sanitize_name(name)] = peak;
        return;
    }

    if(sscanf(
      msg,
      " %[^:]: %fms (%d%%)",
      name,
      &ms,
      &percentage
    )==3){
      //Silently ignore
      return;
    }

    if(sscanf(
      msg,
      " %[^:]: %fms (%d%%)",
      name,
      &ms,
      &percentage
    )==3){
      //Silently ignore
      return;
    }

    //std::cout << "no match:" << msg;
}

void print_mem_stats(Func &f){
  for(auto& it: mem_sizes){
    std::cout << it.first << ": " << it.second << std::endl;
  }
}

int get_mem_size(Func &f, const char* buf_name){
  if(mem_sizes.find(buf_name)==mem_sizes.end())
    return -1;
  return mem_sizes[buf_name];
}

void trace_mem(Func &f){
  f.set_custom_print(&collect_mem_stats);
}

void reset_mem_trace(Func &f){
  mem_sizes.clear();
}

void reset_stats(Func &f){
  reset_counters(f);
  reset_mem_trace(f);
}

//set global pointer to default print function
std::function<void(const char*)> print_fn;

// set custom print function
void custom_print(void* ptr, const char* s){
  //call global print function
  print_fn(s);
}

void set_custom_print(Func &f, std::function<void(const char*)> &print){
  // override global print function
  print_fn = print;

  //register wrapper with void*
  f.set_custom_print(&custom_print);
}


void define_func(py::module &m) {
    define_func_ref(m);
    define_var_or_rvar(m);
    define_loop_level(m);

    // TODO: ParamMap to its own file?
    auto param_map_class = py::class_<ParamMap>(m, "ParamMap")
        .def(py::init<>())
    ;

    // Deliberately not supported, because they don't seem to make sense for Python:
    // - set_custom_allocator()
    // - set_custom_do_task()
    // - set_custom_do_par_for()
    // - jit_handlers()
    // - add_custom_lowering_pass()
    // - clear_custom_lowering_passes()
    // - custom_lowering_passes()

    // Not supported yet, because we want to think about how to expose runtime
    // overrides in Python (https://github.com/halide/Halide/issues/2790):
    // - set_error_handler()
    // - set_custom_trace()
    // - set_custom_print()

    auto func_class = py::class_<Func>(m, "Func")
        .def(py::init<>())
        .def(py::init<std::string>())
        .def(py::init<Expr>())
        .def(py::init([](Buffer<> &b) -> Func { return Func(b); }))

        // for implicitly_convertible
        .def(py::init([](const ImageParam &im) -> Func { return im; }))

        .def("realize", [](Func &f, Buffer<> buffer, const Target &target, const ParamMap &param_map) -> void {
            f.realize(buffer, target);
        }, py::arg("dst"), py::arg("target") = Target(), py::arg("param_map") = ParamMap())

        // This will actually allow a list-of-buffers as well as a tuple-of-buffers, but that's OK.
        .def("realize", [](Func &f, std::vector<Buffer<>> buffers, const Target &t, const ParamMap &param_map) -> void {
            f.realize(Realization(buffers), t);
        }, py::arg("dst"), py::arg("target") = Target(), py::arg("param_map") = ParamMap())

        .def("realize", [](Func &f, std::vector<int32_t> sizes, const Target &target, const ParamMap &param_map) -> py::object {
            return realization_to_object(f.realize(sizes, target, param_map));
        }, py::arg("sizes") = std::vector<int32_t>{}, py::arg("target") = Target(), py::arg("param_map") = ParamMap())

        // TODO: deprecate in favor of std::vector<int32_t> size version?
        .def("realize", [](Func &f, int x_size, const Target &target, const ParamMap &param_map) -> py::object {
            return realization_to_object(f.realize(x_size, target, param_map));
        }, py::arg("x_size"), py::arg("target") = Target(), py::arg("param_map") = ParamMap())

        // TODO: deprecate in favor of std::vector<int32_t> size version?
        .def("realize", [](Func &f, int x_size, int y_size, const Target &target, const ParamMap &param_map) -> py::object {
            return realization_to_object(f.realize(x_size, y_size, target, param_map));
        }, py::arg("x_size"), py::arg("y_size"), py::arg("target") = Target(), py::arg("param_map") = ParamMap())

        // TODO: deprecate in favor of std::vector<int32_t> size version?
        .def("realize", [](Func &f, int x_size, int y_size, int z_size, const Target &target, const ParamMap &param_map) -> py::object {
            return realization_to_object(f.realize(x_size, y_size, z_size, target, param_map));
        }, py::arg("x_size"), py::arg("y_size"), py::arg("z_size"), py::arg("target") = Target(), py::arg("param_map") = ParamMap())

        // TODO: deprecate in favor of std::vector<int32_t> size version?
        .def("realize", [](Func &f, int x_size, int y_size, int z_size, int w_size, const Target &target, const ParamMap &param_map) -> py::object {
            return realization_to_object(f.realize(x_size, y_size, z_size, w_size, target, param_map));
        }, py::arg("x_size"), py::arg("y_size"), py::arg("z_size"), py::arg("w_size"), py::arg("target") = Target(), py::arg("param_map") = ParamMap())

        .def("defined", &Func::defined)
        .def("name", &Func::name)
        .def("dimensions", &Func::dimensions)
        .def("args", &Func::args)
        .def("value", &Func::value)
        .def("values", [](Func &func) -> py::tuple {
            return to_python_tuple(func.values());
        })
        .def("defined", &Func::defined)
        .def("outputs", &Func::outputs)
        .def("output_types", &Func::output_types)

        .def("bound", &Func::bound, py::arg("var"), py::arg("min"), py::arg("extent"))

        .def("reorder_storage", (Func &(Func::*)(const std::vector<Var> &)) &Func::reorder_storage,
            py::arg("dims"))
        .def("reorder_storage", [](Func &func, py::args args) -> Func & {
            return func.reorder_storage(args_to_vector<Var>(args));
        })

        .def("compute_at", (Func &(Func::*)(Func, Var)) &Func::compute_at,
            py::arg("f"), py::arg("var"))
        .def("compute_at", (Func &(Func::*)(Func, RVar)) &Func::compute_at,
            py::arg("f"), py::arg("var"))
        .def("compute_at", (Func &(Func::*)(LoopLevel)) &Func::compute_at,
            py::arg("loop_level"))

        .def("store_at", (Func &(Func::*)(Func, Var)) &Func::store_at,
            py::arg("f"), py::arg("var"))
        .def("store_at", (Func &(Func::*)(Func, RVar)) &Func::store_at,
            py::arg("f"), py::arg("var"))
        .def("store_at", (Func &(Func::*)(LoopLevel)) &Func::store_at,
            py::arg("loop_level"))

        .def("memoize", &Func::memoize)
        .def("compute_inline", &Func::compute_inline)
        .def("compute_root", &Func::compute_root)
        .def("store_root", &Func::store_root)

        .def("store_in", &Func::store_in,
            py::arg("memory_type"))

        .def("compile_to", &Func::compile_to,
            py::arg("outputs"), py::arg("arguments"), py::arg("fn_name"), py::arg("target") = get_target_from_environment())

        .def("compile_to_bitcode",
            (void (Func::*)(const std::string &, const std::vector<Argument> &, const std::string &, const Target &target)) &Func::compile_to_bitcode,
            py::arg("filename"), py::arg("arguments"), py::arg("fn_name"), py::arg("target") = get_target_from_environment())
        .def("compile_to_bitcode",
            (void (Func::*)(const std::string &, const std::vector<Argument> &, const Target &target)) &Func::compile_to_bitcode,
            py::arg("filename"), py::arg("arguments"), py::arg("target") = get_target_from_environment())

        .def("compile_to_llvm_assembly",
            (void (Func::*)(const std::string &, const std::vector<Argument> &, const std::string &, const Target &target)) &Func::compile_to_llvm_assembly,
            py::arg("filename"), py::arg("arguments"), py::arg("fn_name"), py::arg("target") = get_target_from_environment())
        .def("compile_to_llvm_assembly",
            (void (Func::*)(const std::string &, const std::vector<Argument> &, const Target &target)) &Func::compile_to_llvm_assembly,
            py::arg("filename"), py::arg("arguments"), py::arg("target") = get_target_from_environment())

        .def("compile_to_object",
            (void (Func::*)(const std::string &, const std::vector<Argument> &, const std::string &, const Target &target)) &Func::compile_to_object,
            py::arg("filename"), py::arg("arguments"), py::arg("fn_name"), py::arg("target") = get_target_from_environment())
        .def("compile_to_object",
            (void (Func::*)(const std::string &, const std::vector<Argument> &, const Target &target)) &Func::compile_to_object,
            py::arg("filename"), py::arg("arguments"), py::arg("target") = get_target_from_environment())

        .def("compile_to_header", &Func::compile_to_header,
            py::arg("filename"), py::arg("arguments"), py::arg("fn_name") = "", py::arg("target") = get_target_from_environment())

        .def("compile_to_assembly",
            (void (Func::*)(const std::string &, const std::vector<Argument> &, const std::string &, const Target &target)) &Func::compile_to_assembly,
            py::arg("filename"), py::arg("arguments"), py::arg("fn_name"), py::arg("target") = get_target_from_environment())
        .def("compile_to_assembly",
            (void (Func::*)(const std::string &, const std::vector<Argument> &, const Target &target)) &Func::compile_to_assembly,
            py::arg("filename"), py::arg("arguments"), py::arg("target") = get_target_from_environment())

        .def("compile_to_c", &Func::compile_to_c,
            py::arg("filename"), py::arg("arguments"), py::arg("fn_name") = "", py::arg("target") = get_target_from_environment())

        .def("compile_to_lowered_stmt", &Func::compile_to_lowered_stmt,
            py::arg("filename"), py::arg("arguments"), py::arg("fmt") = Text, py::arg("target") = get_target_from_environment())

        .def("compile_to_file", &Func::compile_to_file,
            py::arg("filename_prefix"), py::arg("arguments"), py::arg("fn_name") = "", py::arg("target") = get_target_from_environment())

        .def("compile_to_python_extension", &Func::compile_to_python_extension,
            py::arg("filename_prefix"), py::arg("arguments"), py::arg("fn_name") = "", py::arg("target") = get_target_from_environment())

        .def("compile_to_static_library", &Func::compile_to_static_library,
            py::arg("filename_prefix"), py::arg("arguments"), py::arg("fn_name") = "", py::arg("target") = get_target_from_environment())

        .def("compile_to_multitarget_static_library", &Func::compile_to_multitarget_static_library,
            py::arg("filename_prefix"), py::arg("arguments"), py::arg("targets"))

        // TODO: useless until Module is defined.
        .def("compile_to_module", &Func::compile_to_module,
            py::arg("arguments"), py::arg("fn_name") = "", py::arg("target") = get_target_from_environment())

        .def("compile_jit", &Func::compile_jit, py::arg("target") = get_jit_target_from_environment())

        .def("has_update_definition", &Func::has_update_definition)
        .def("num_update_definitions", &Func::num_update_definitions)

        .def("update", &Func::update, py::arg("idx") = 0)
        .def("update_args", &Func::update_args, py::arg("idx") = 0)
        .def("update_value", &Func::update_value, py::arg("idx") = 0)
        .def("update_value", &Func::update_value, py::arg("idx") = 0)
        .def("update_values", [](Func &func, int idx) -> py::tuple {
            return to_python_tuple(func.update_values(idx));
        }, py::arg("idx") = 0)
        .def("rvars", &Func::rvars, py::arg("idx") = 0)


        .def("trace_mem", &trace_mem) // trace memory sizes
        .def("get_mem_size", &get_mem_size) // get size of buffer name
        .def("print_mem_stats", &print_mem_stats) //print all traced memories
        .def("reset_traces", &reset_stats)

        .def("set_custom_print", &set_custom_print) // set custom print functionk
        .def("get_loads", &get_loads)           // get loads for <buffer name>
        .def("get_stores", &get_stores)         // get stores for <buffer name>
        .def("print_counters", &print_counters) // print all counter values
        .def("init_counter", &init_counter_ext) // initialize <buffer name> counters with zeros
        .def("count_accesses", &register_count_accesses) //inits counters if not used yet
        .def("count_accesses_unsafe", &register_count_accesses_unsafe)  //inits have to be done by user


        .def("trace_loads", &Func::trace_loads)
        .def("trace_stores", &Func::trace_stores)
        .def("trace_realizations", &Func::trace_realizations)
        .def("print_loop_nest", &Func::print_loop_nest)
        .def("add_trace_tag", &Func::add_trace_tag, py::arg("trace_tag"))

        // TODO: also provide to-array versions to avoid requiring filesystem usage
        .def("debug_to_file", &Func::debug_to_file)

        .def("is_extern", &Func::is_extern)
        .def("extern_function_name", &Func::extern_function_name)

        .def("define_extern", (void (Func::*)(const std::string &, const std::vector<ExternFuncArgument> &,
                const std::vector<Type> &, const std::vector<Var> &, NameMangling, DeviceAPI)) &Func::define_extern,
             py::arg("function_name"), py::arg("params"), py::arg("types"),
             py::arg("arguments"), py::arg("mangling") = NameMangling::Default,
             py::arg("device_api") = DeviceAPI::Host)

        .def("define_extern", (void (Func::*)(const std::string &, const std::vector<ExternFuncArgument> &,
                Type, int, NameMangling, DeviceAPI)) &Func::define_extern,
             py::arg("function_name"), py::arg("params"), py::arg("type"),
             py::arg("dimensionality"), py::arg("mangling") = NameMangling::Default,
             py::arg("device_api") = DeviceAPI::Host)

        .def("define_extern", (void (Func::*)(const std::string &, const std::vector<ExternFuncArgument> &,
                const std::vector<Type> &, int, NameMangling, DeviceAPI)) &Func::define_extern,
             py::arg("function_name"), py::arg("params"), py::arg("types"),
             py::arg("dimensionality"), py::arg("mangling") = NameMangling::Default,
             py::arg("device_api") = DeviceAPI::Host)

        .def("define_extern", (void (Func::*)(const std::string &, const std::vector<ExternFuncArgument> &,
                Type, const std::vector<Var> &, NameMangling, DeviceAPI)) &Func::define_extern,
             py::arg("function_name"), py::arg("params"), py::arg("type"),
             py::arg("arguments"), py::arg("mangling") = NameMangling::Default,
             py::arg("device_api") = DeviceAPI::Host)

        .def("output_buffer", &Func::output_buffer)
        .def("output_buffers", &Func::output_buffers)

        .def("infer_input_bounds", (void (Func::*)(int, int, int, int, const ParamMap &)) &Func::infer_input_bounds,
            py::arg("x_size") = 0, py::arg("y_size") = 0, py::arg("z_size") = 0, py::arg("w_size") = 0, py::arg("param_map") = ParamMap())

        .def("infer_input_bounds", [](Func &f, Buffer<> buffer, const ParamMap &param_map) -> void {
            f.infer_input_bounds(buffer, param_map);
        }, py::arg("dst"), py::arg("param_map") = ParamMap())

        .def("infer_input_bounds", [](Func &f, std::vector<Buffer<>> buffer, const ParamMap &param_map) -> void {
            f.infer_input_bounds(Realization(buffer), param_map);
        }, py::arg("dst"), py::arg("param_map") = ParamMap())

        .def("in", (Func (Func::*)(const Func &)) &Func::in, py::arg("f"))
        .def("in", (Func (Func::*)(const std::vector<Func> &fs)) &Func::in, py::arg("fs"))
        .def("in", (Func (Func::*)()) &Func::in)

        .def("clone_in", (Func (Func::*)(const Func &)) &Func::clone_in, py::arg("f"))
        .def("clone_in", (Func (Func::*)(const std::vector<Func> &fs)) &Func::clone_in, py::arg("fs"))

        .def("copy_to_device", &Func::copy_to_device,
            py::arg("device_api") = DeviceAPI::Default_GPU)
        .def("copy_to_host", &Func::copy_to_host)

        .def("set_estimate", &Func::set_estimate,
            py::arg("var"), py::arg("min"), py::arg("extent"))
        .def("set_estimates", &Func::set_estimates,
            py::arg("estimates"))

        .def("align_bounds", &Func::align_bounds,
            py::arg("var"), py::arg("modulus"), py::arg("remainder") = 0)

        .def("bound_extent", &Func::bound_extent,
            py::arg("var"), py::arg("extent"))

        .def("gpu_lanes", &Func::gpu_lanes,
            py::arg("thread_x"), py::arg("device_api") = DeviceAPI::Default_GPU)

        .def("shader", &Func::shader,
            py::arg("x"), py::arg("y"), py::arg("c"), py::arg("device_api"))

        .def("glsl", &Func::glsl,
            py::arg("x"), py::arg("y"), py::arg("c"))

        .def("align_storage", &Func::align_storage,
            py::arg("dim"), py::arg("alignment"))

        .def("fold_storage", &Func::fold_storage,
            py::arg("dim"), py::arg("extent"), py::arg("fold_forward") = true)

        .def("compute_with", (Func &(Func::*)(LoopLevel, const std::vector<std::pair<VarOrRVar, LoopAlignStrategy>> &)) &Func::compute_with,
            py::arg("loop_level"), py::arg("align"))
        .def("compute_with", (Func &(Func::*)(LoopLevel, LoopAlignStrategy)) &Func::compute_with,
            py::arg("loop_level"), py::arg("align") = LoopAlignStrategy::Auto)

        .def("infer_arguments", &Func::infer_arguments)

        .def("__repr__", [](const Func &func) -> std::string {
            std::ostringstream o;
            o << "<halide.Func '" << func.name() << "'>";
            return o.str();
        })
    ;

    py::implicitly_convertible<ImageParam, Func>();

    // Note that overloads of FuncRef must come *before* Expr;
    // otherwise PyBind's automatic STL conversion machinery
    // can attempt to convert a FuncRef into a vector-of-size-1 Expr,
    // which will fail. TODO: can we avoid this?

    // Ordinary calls to Funcs
    define_get<FuncRef>(func_class);
    define_get<Expr>(func_class);
    define_get<std::vector<Expr>>(func_class);
    define_get<Var>(func_class);
    define_get<std::vector<Var>>(func_class);

    // LHS(Var, ...Var) is LHS of an ordinary Func definition.
    define_set<Var, FuncRef>(func_class);
    define_set<Var, Expr>(func_class);
    define_set<Var, Tuple>(func_class);
    //define_set<Var, std::vector<Var>>(func_class);

    // LHS(Expr, ...Expr) can only be LHS of an update definition.
    define_set<Expr, FuncRef>(func_class);
    define_set<Expr, Expr>(func_class);
    define_set<Expr, Tuple>(func_class);

    add_schedule_methods(func_class);

    py::implicitly_convertible<ImageParam, Func>();

    define_stage(m);
}

}  // namespace PythonBindings
}  // namespace Halide
