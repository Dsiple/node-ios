#include "env-inl.h"
#include "node_internals.h"
#include "node_options-inl.h"
#include "node_metadata.h"
#include "node_process.h"
#include "node_revert.h"
#include "util-inl.h"

#include <climits>  // PATH_MAX

namespace node {
using v8::Context;
using v8::DEFAULT;
using v8::EscapableHandleScope;
using v8::Function;
using v8::FunctionTemplate;
using v8::HandleScope;
using v8::Integer;
using v8::Isolate;
using v8::Just;
using v8::Local;
using v8::MaybeLocal;
using v8::Name;
using v8::NewStringType;
using v8::None;
using v8::Object;
using v8::PropertyCallbackInfo;
using v8::SideEffectType;
using v8::String;
using v8::Value;

static void ProcessTitleGetter(Local<Name> property,
                               const PropertyCallbackInfo<Value>& info) {
  char buffer[512];
  uv_get_process_title(buffer, sizeof(buffer));
  info.GetReturnValue().Set(
      String::NewFromUtf8(info.GetIsolate(), buffer, NewStringType::kNormal)
          .ToLocalChecked());
}

static void ProcessTitleSetter(Local<Name> property,
                               Local<Value> value,
                               const PropertyCallbackInfo<void>& info) {
  node::Utf8Value title(info.GetIsolate(), value);
  TRACE_EVENT_METADATA1(
      "__metadata", "process_name", "name", TRACE_STR_COPY(*title));
  uv_set_process_title(*title);
}

static void DebugPortGetter(Local<Name> property,
                            const PropertyCallbackInfo<Value>& info) {
  Environment* env = Environment::GetCurrent(info);
  int port = env->inspector_host_port()->port();
  info.GetReturnValue().Set(port);
}

static void DebugPortSetter(Local<Name> property,
                            Local<Value> value,
                            const PropertyCallbackInfo<void>& info) {
  Environment* env = Environment::GetCurrent(info);
  int32_t port = value->Int32Value(env->context()).FromMaybe(0);
  env->inspector_host_port()->set_port(static_cast<int>(port));
}

static void GetParentProcessId(Local<Name> property,
                               const PropertyCallbackInfo<Value>& info) {
  info.GetReturnValue().Set(uv_os_getppid());
}

MaybeLocal<Object> CreateProcessObject(
    Environment* env,
    const std::vector<std::string>& args,
    const std::vector<std::string>& exec_args) {
  Isolate* isolate = env->isolate();
  EscapableHandleScope scope(isolate);
  Local<Context> context = env->context();

  Local<FunctionTemplate> process_template = FunctionTemplate::New(isolate);
  process_template->SetClassName(FIXED_ONE_BYTE_STRING(isolate, "process"));
  Local<Function> process_ctor;
  Local<Object> process;
  if (!process_template->GetFunction(context).ToLocal(&process_ctor) ||
      !process_ctor->NewInstance(context).ToLocal(&process)) {
    return MaybeLocal<Object>();
  }

  // process.title
  auto title_string = FIXED_ONE_BYTE_STRING(env->isolate(), "title");
  CHECK(process
            ->SetAccessor(
                env->context(),
                title_string,
                ProcessTitleGetter,
                env->owns_process_state() ? ProcessTitleSetter : nullptr,
                env->as_callback_data(),
                DEFAULT,
                None,
                SideEffectType::kHasNoSideEffect)
            .FromJust());

  // process.version
  READONLY_PROPERTY(process,
                    "version",
                    FIXED_ONE_BYTE_STRING(env->isolate(), NODE_VERSION));

  // process.versions
  Local<Object> versions = Object::New(env->isolate());
  READONLY_PROPERTY(process, "versions", versions);

#define V(key)                                                                 \
  if (!per_process::metadata.versions.key.empty()) {                           \
    READONLY_STRING_PROPERTY(                                                  \
        versions, #key, per_process::metadata.versions.key);                   \
  }
  NODE_VERSIONS_KEYS(V)
#undef V

  // process.arch
  READONLY_STRING_PROPERTY(process, "arch", per_process::metadata.arch);

  // process.platform
  READONLY_STRING_PROPERTY(process, "platform", per_process::metadata.platform);

  // process.release
  Local<Object> release = Object::New(env->isolate());
  READONLY_PROPERTY(process, "release", release);
  READONLY_STRING_PROPERTY(release, "name", per_process::metadata.release.name);
#if NODE_VERSION_IS_LTS
  READONLY_STRING_PROPERTY(release, "lts", per_process::metadata.release.lts);
#endif  // NODE_VERSION_IS_LTS

#ifdef NODE_HAS_RELEASE_URLS
  READONLY_STRING_PROPERTY(
      release, "sourceUrl", per_process::metadata.release.source_url);
  READONLY_STRING_PROPERTY(
      release, "headersUrl", per_process::metadata.release.headers_url);
#ifdef _WIN32
  READONLY_STRING_PROPERTY(
      release, "libUrl", per_process::metadata.release.lib_url);
#endif  // _WIN32
#endif  // NODE_HAS_RELEASE_URLS

  // process.argv
  process->Set(env->context(),
               FIXED_ONE_BYTE_STRING(env->isolate(), "argv"),
               ToV8Value(env->context(), args).ToLocalChecked()).FromJust();

  // process.execArgv
  process->Set(env->context(),
               FIXED_ONE_BYTE_STRING(env->isolate(), "execArgv"),
               ToV8Value(env->context(), exec_args)
                   .ToLocalChecked()).FromJust();

  READONLY_PROPERTY(process, "pid",
                    Integer::New(env->isolate(), uv_os_getpid()));

  CHECK(process->SetAccessor(env->context(),
                             FIXED_ONE_BYTE_STRING(env->isolate(), "ppid"),
                             GetParentProcessId).FromJust());

  // TODO(joyeecheung): make this available in JS during pre-execution.
  // Note that to use this in releases the code doing the revert need to be
  // careful to delay the check until after the bootstrap but that may not
  // be possible depending on the feature being reverted.

  // --security-revert flags
#define V(code, _, __)                                                        \
  do {                                                                        \
    if (IsReverted(SECURITY_REVERT_ ## code)) {                               \
      READONLY_PROPERTY(process, "REVERT_" #code, True(env->isolate()));      \
    }                                                                         \
  } while (0);
  SECURITY_REVERSIONS(V)
#undef V

  // process.execPath
  {
    char exec_path_buf[2 * PATH_MAX];
    size_t exec_path_len = sizeof(exec_path_buf);
    std::string exec_path;
    if (uv_exepath(exec_path_buf, &exec_path_len) == 0) {
      exec_path = std::string(exec_path_buf, exec_path_len);
    } else {
      exec_path = args[0];
    }
    // On OpenBSD process.execPath will be relative unless we
    // get the full path before process.execPath is used.
#if defined(__OpenBSD__)
    uv_fs_t req;
    req.ptr = nullptr;
    if (0 ==
        uv_fs_realpath(env->event_loop(), &req, exec_path.c_str(), nullptr)) {
      CHECK_NOT_NULL(req.ptr);
      exec_path = std::string(static_cast<char*>(req.ptr));
    }
#endif
    process
        ->Set(env->context(),
              FIXED_ONE_BYTE_STRING(env->isolate(), "execPath"),
              String::NewFromUtf8(env->isolate(),
                                  exec_path.c_str(),
                                  NewStringType::kInternalized,
                                  exec_path.size())
                  .ToLocalChecked())
        .FromJust();
  }

  // process.debugPort
  auto debug_port_string = FIXED_ONE_BYTE_STRING(env->isolate(), "debugPort");
  CHECK(process
            ->SetAccessor(env->context(),
                          debug_port_string,
                          DebugPortGetter,
                          env->owns_process_state() ? DebugPortSetter : nullptr,
                          env->as_callback_data())
            .FromJust());

  // process._rawDebug: may be overwritten later in JS land, but should be
  // availbale from the begining for debugging purposes
  env->SetMethod(process, "_rawDebug", RawDebug);

  return scope.Escape(process);
}

void GetLoop(const v8::FunctionCallbackInfo<Value>& args) {
  uv_loop_t *loop = Environment::GetThreadLocalLoop();
  uintptr_t loopPtr = (uintptr_t )loop;
  uint32_t a = (uint32_t)((loopPtr >> 32) & 0xFFFFFFFF);
  uint32_t b = (uint32_t)(loopPtr & 0xFFFFFFFF);

  Isolate* isolate = Environment::GetThreadLocalIsolate();
  Local<v8::Array> result = v8::Array::New(isolate, 2);
  result->Set(0, Integer::NewFromUnsigned(isolate, a));
  result->Set(1, Integer::NewFromUnsigned(isolate, b));
  args.GetReturnValue().Set(result);
}

}  // namespace node
