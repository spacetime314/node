#include "node_main_instance.h"
#include "node_internals.h"
#include "node_options-inl.h"
#include "node_v8_platform-inl.h"

namespace node {

using v8::Context;
using v8::HandleScope;
using v8::Isolate;
using v8::Local;
using v8::Locker;
using v8::SealHandleScope;

NodeMainInstance::NodeMainInstance(uv_loop_t* event_loop,
                                   const std::vector<std::string>& args,
                                   const std::vector<std::string>& exec_args)
    : args_(args),
      exec_args_(exec_args),
      array_buffer_allocator_(ArrayBufferAllocator::Create()),
      isolate_(nullptr),
      isolate_data_(nullptr) {
  // TODO(joyeecheung): when we implement snapshot integration this needs to
  // set params.external_references.
  Isolate::CreateParams params;
  params.array_buffer_allocator = array_buffer_allocator_.get();
  isolate_ =
      NewIsolate(&params, event_loop, per_process::v8_platform.Platform());
  CHECK_NOT_NULL(isolate_);
  isolate_data_.reset(CreateIsolateData(isolate_,
                                        event_loop,
                                        per_process::v8_platform.Platform(),
                                        array_buffer_allocator_.get()));
}

NodeMainInstance::~NodeMainInstance() {
  isolate_->Dispose();
  per_process::v8_platform.Platform()->UnregisterIsolate(isolate_);
}

int NodeMainInstance::Run() {
  Locker locker(isolate_);
  Isolate::Scope isolate_scope(isolate_);
  HandleScope handle_scope(isolate_);

  int exit_code = 0;
  std::unique_ptr<Environment> env = CreateMainEnvironment(&exit_code);

  CHECK_NOT_NULL(env);
  Context::Scope context_scope(env->context());

  if (exit_code == 0) {
    {
      AsyncCallbackScope callback_scope(env.get());
      env->async_hooks()->push_async_ids(1, 0);
      LoadEnvironment(env.get());
      env->async_hooks()->pop_async_id(1);
    }

    {
      SealHandleScope seal(isolate_);
      bool more;
      env->performance_state()->Mark(
          node::performance::NODE_PERFORMANCE_MILESTONE_LOOP_START);
      do {
        uv_run(env->event_loop(), UV_RUN_DEFAULT);

        per_process::v8_platform.DrainVMTasks(isolate_);

        more = uv_loop_alive(env->event_loop());
        if (more && !env->is_stopping()) continue;

        env->RunBeforeExitCallbacks();

        if (!uv_loop_alive(env->event_loop())) {
          EmitBeforeExit(env.get());
        }

        // Emit `beforeExit` if the loop became alive either after emitting
        // event, or after running some callbacks.
        more = uv_loop_alive(env->event_loop());
      } while (more == true && !env->is_stopping());
      env->performance_state()->Mark(
          node::performance::NODE_PERFORMANCE_MILESTONE_LOOP_EXIT);
    }

    env->set_trace_sync_io(false);
    exit_code = EmitExit(env.get());
    WaitForInspectorDisconnect(env.get());
  }

  env->set_can_call_into_js(false);
  env->stop_sub_worker_contexts();
  uv_tty_reset_mode();
  env->RunCleanup();
  RunAtExit(env.get());

  per_process::v8_platform.DrainVMTasks(isolate_);
  per_process::v8_platform.CancelVMTasks(isolate_);

#if defined(LEAK_SANITIZER)
  __lsan_do_leak_check();
#endif

  return exit_code;
}

// TODO(joyeecheung): align this with the CreateEnvironment exposed in node.h
// and the environment creation routine in workers somehow.
std::unique_ptr<Environment> NodeMainInstance::CreateMainEnvironment(
    int* exit_code) {
  *exit_code = 0;  // Reset the exit code to 0

  HandleScope handle_scope(isolate_);

  // TODO(addaleax): This should load a real per-Isolate option, currently
  // this is still effectively per-process.
  if (isolate_data_->options()->track_heap_objects) {
    isolate_->GetHeapProfiler()->StartTrackingHeapObjects(true);
  }

  Local<Context> context = NewContext(isolate_);
  Context::Scope context_scope(context);

  std::unique_ptr<Environment> env = std::make_unique<Environment>(
      isolate_data_.get(),
      context,
      static_cast<Environment::Flags>(Environment::kIsMainThread |
                                      Environment::kOwnsProcessState |
                                      Environment::kOwnsInspector));
  env->InitializeLibuv(per_process::v8_is_profiling);
  env->ProcessCliArgs(args_, exec_args_);

#if HAVE_INSPECTOR && NODE_USE_V8_PLATFORM
  CHECK(!env->inspector_agent()->IsListening());
  // Inspector agent can't fail to start, but if it was configured to listen
  // right away on the websocket port and fails to bind/etc, this will return
  // false.
  env->inspector_agent()->Start(args_.size() > 1 ? args_[1].c_str() : "",
                                env->options()->debug_options(),
                                env->inspector_host_port(),
                                true);
  if (env->options()->debug_options().inspector_enabled &&
      !env->inspector_agent()->IsListening()) {
    *exit_code = 12;  // Signal internal error.
    return env;
  }
#else
  // inspector_enabled can't be true if !HAVE_INSPECTOR or
  // !NODE_USE_V8_PLATFORM
  // - the option parser should not allow that.
  CHECK(!env->options()->debug_options().inspector_enabled);
#endif  // HAVE_INSPECTOR && NODE_USE_V8_PLATFORM

  if (RunBootstrapping(env.get()).IsEmpty()) {
    *exit_code = 1;
  }

  return env;
}

}  // namespace node
