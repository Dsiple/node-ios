// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef SRC_NODE_INTERNALS_H_
#define SRC_NODE_INTERNALS_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include "env-inl.h"
#include "node.h"
#include "node_binding.h"
#include "node_mutex.h"
#include "node_persistent.h"
#include "tracing/trace_event.h"
#include "util-inl.h"
#include "uv.h"
#include "v8.h"

#include <cstdint>
#include <cstdlib>

#include <string>
#include <vector>

// Custom constants used by both node_constants.cc and node_zlib.cc
#define Z_MIN_WINDOWBITS 8
#define Z_MAX_WINDOWBITS 15
#define Z_DEFAULT_WINDOWBITS 15

struct sockaddr;

namespace node {

namespace native_module {
class NativeModuleLoader;
}

namespace per_process {
extern Mutex env_var_mutex;
extern uint64_t node_start_time;
extern bool v8_is_profiling;
}  // namespace per_process

// Forward declaration
class Environment;

// Convert a struct sockaddr to a { address: '1.2.3.4', port: 1234 } JS object.
// Sets address and port properties on the info object and returns it.
// If |info| is omitted, a new object is returned.
v8::Local<v8::Object> AddressToJS(
    Environment* env,
    const sockaddr* addr,
    v8::Local<v8::Object> info = v8::Local<v8::Object>());

template <typename T, int (*F)(const typename T::HandleType*, sockaddr*, int*)>
void GetSockOrPeerName(const v8::FunctionCallbackInfo<v8::Value>& args) {
  T* wrap;
  ASSIGN_OR_RETURN_UNWRAP(&wrap,
                          args.Holder(),
                          args.GetReturnValue().Set(UV_EBADF));
  CHECK(args[0]->IsObject());
  sockaddr_storage storage;
  int addrlen = sizeof(storage);
  sockaddr* const addr = reinterpret_cast<sockaddr*>(&storage);
  const int err = F(&wrap->handle_, addr, &addrlen);
  if (err == 0)
    AddressToJS(wrap->env(), addr, args[0].As<v8::Object>());
  args.GetReturnValue().Set(err);
}

void WaitForInspectorDisconnect(Environment* env);
void SignalExit(int signo);
#ifdef __POSIX__
void RegisterSignalHandler(int signal,
                           void (*handler)(int signal),
                           bool reset_handler = false);
#endif

std::string GetHumanReadableProcessName();
void GetHumanReadableProcessName(char (*name)[1024]);

namespace task_queue {
void PromiseRejectCallback(v8::PromiseRejectMessage message);
}  // namespace task_queue

class NodeArrayBufferAllocator : public ArrayBufferAllocator {
 public:
  inline uint32_t* zero_fill_field() { return &zero_fill_field_; }

  void* Allocate(size_t size) override;  // Defined in src/node.cc
  void* AllocateUninitialized(size_t size) override
    { return node::UncheckedMalloc(size); }
  void Free(void* data, size_t) override { free(data); }
  virtual void* Reallocate(void* data, size_t old_size, size_t size) {
    return static_cast<void*>(
        UncheckedRealloc<char>(static_cast<char*>(data), size));
  }
  virtual void RegisterPointer(void* data, size_t size) {}
  virtual void UnregisterPointer(void* data, size_t size) {}

  NodeArrayBufferAllocator* GetImpl() final { return this; }

 private:
  uint32_t zero_fill_field_ = 1;  // Boolean but exposed as uint32 to JS land.
};

class DebuggingArrayBufferAllocator final : public NodeArrayBufferAllocator {
 public:
  ~DebuggingArrayBufferAllocator() override;
  void* Allocate(size_t size) override;
  void* AllocateUninitialized(size_t size) override;
  void Free(void* data, size_t size) override;
  void* Reallocate(void* data, size_t old_size, size_t size) override;
  void RegisterPointer(void* data, size_t size) override;
  void UnregisterPointer(void* data, size_t size) override;

 private:
  void RegisterPointerInternal(void* data, size_t size);
  void UnregisterPointerInternal(void* data, size_t size);
  Mutex mutex_;
  std::unordered_map<void*, size_t> allocations_;
};

namespace Buffer {
v8::MaybeLocal<v8::Object> Copy(Environment* env, const char* data, size_t len);
v8::MaybeLocal<v8::Object> New(Environment* env, size_t size);
// Takes ownership of |data|.
v8::MaybeLocal<v8::Object> New(Environment* env,
                               char* data,
                               size_t length,
                               void (*callback)(char* data, void* hint),
                               void* hint);
// Takes ownership of |data|.  Must allocate |data| with the current Isolate's
// ArrayBuffer::Allocator().
v8::MaybeLocal<v8::Object> New(Environment* env,
                               char* data,
                               size_t length,
                               bool uses_malloc);

// Construct a Buffer from a MaybeStackBuffer (and also its subclasses like
// Utf8Value and TwoByteValue).
// If |buf| is invalidated, an empty MaybeLocal is returned, and nothing is
// changed.
// If |buf| contains actual data, this method takes ownership of |buf|'s
// underlying buffer. However, |buf| itself can be reused even after this call,
// but its capacity, if increased through AllocateSufficientStorage, is not
// guaranteed to stay the same.
template <typename T>
static v8::MaybeLocal<v8::Object> New(Environment* env,
                                      MaybeStackBuffer<T>* buf) {
  v8::MaybeLocal<v8::Object> ret;
  char* src = reinterpret_cast<char*>(buf->out());
  const size_t len_in_bytes = buf->length() * sizeof(buf->out()[0]);

  if (buf->IsAllocated())
    ret = New(env, src, len_in_bytes, true);
  else if (!buf->IsInvalidated())
    ret = Copy(env, src, len_in_bytes);

  if (ret.IsEmpty())
    return ret;

  if (buf->IsAllocated())
    buf->Release();

  return ret;
}
}  // namespace Buffer

v8::MaybeLocal<v8::Value> InternalMakeCallback(
    Environment* env,
    v8::Local<v8::Object> recv,
    const v8::Local<v8::Function> callback,
    int argc,
    v8::Local<v8::Value> argv[],
    async_context asyncContext);

class InternalCallbackScope {
 public:
  // Tell the constructor whether its `object` parameter may be empty or not.
  enum ResourceExpectation { kRequireResource, kAllowEmptyResource };
  InternalCallbackScope(Environment* env,
                        v8::Local<v8::Object> object,
                        const async_context& asyncContext,
                        ResourceExpectation expect = kRequireResource);
  // Utility that can be used by AsyncWrap classes.
  explicit InternalCallbackScope(AsyncWrap* async_wrap);
  ~InternalCallbackScope();
  void Close();

  inline bool Failed() const { return failed_; }
  inline void MarkAsFailed() { failed_ = true; }

 private:
  Environment* env_;
  async_context async_context_;
  v8::Local<v8::Object> object_;
  AsyncCallbackScope callback_scope_;
  bool failed_ = false;
  bool pushed_ids_ = false;
  bool closed_ = false;
};

class DebugSealHandleScope {
 public:
  explicit inline DebugSealHandleScope(v8::Isolate* isolate)
#ifdef DEBUG
    : actual_scope_(isolate)
#endif
  {}

 private:
#ifdef DEBUG
  v8::SealHandleScope actual_scope_;
#endif
};

class ThreadPoolWork {
 public:
  explicit inline ThreadPoolWork(Environment* env) : env_(env) {
    CHECK_NOT_NULL(env);
  }
  inline virtual ~ThreadPoolWork() = default;

  inline void ScheduleWork();
  inline int CancelWork();

  virtual void DoThreadPoolWork() = 0;
  virtual void AfterThreadPoolWork(int status) = 0;

 private:
  Environment* env_;
  uv_work_t work_req_;
};

void ThreadPoolWork::ScheduleWork() {
  env_->IncreaseWaitingRequestCounter();
  int status = uv_queue_work(
      env_->event_loop(),
      &work_req_,
      [](uv_work_t* req) {
        ThreadPoolWork* self = ContainerOf(&ThreadPoolWork::work_req_, req);
        self->DoThreadPoolWork();
      },
      [](uv_work_t* req, int status) {
        ThreadPoolWork* self = ContainerOf(&ThreadPoolWork::work_req_, req);
        self->env_->DecreaseWaitingRequestCounter();
        self->AfterThreadPoolWork(status);
      });
  CHECK_EQ(status, 0);
}

int ThreadPoolWork::CancelWork() {
  return uv_cancel(reinterpret_cast<uv_req_t*>(&work_req_));
}

#define TRACING_CATEGORY_NODE "node"
#define TRACING_CATEGORY_NODE1(one)                                           \
    TRACING_CATEGORY_NODE ","                                                 \
    TRACING_CATEGORY_NODE "." #one
#define TRACING_CATEGORY_NODE2(one, two)                                      \
    TRACING_CATEGORY_NODE ","                                                 \
    TRACING_CATEGORY_NODE "." #one ","                                        \
    TRACING_CATEGORY_NODE "." #one "." #two

// Functions defined in node.cc that are exposed via the bootstrapper object

#if defined(__POSIX__) && !defined(__ANDROID__) && !defined(__CloudABI__)
#define NODE_IMPLEMENTS_POSIX_CREDENTIALS 1
#endif  // __POSIX__ && !defined(__ANDROID__) && !defined(__CloudABI__)

namespace credentials {
bool SafeGetenv(const char* key, std::string* text);
}  // namespace credentials

void DefineZlibConstants(v8::Local<v8::Object> target);

void GetLoop(const v8::FunctionCallbackInfo<v8::Value>& args);

v8::MaybeLocal<v8::Value> RunBootstrapping(Environment* env);
v8::MaybeLocal<v8::Value> StartExecution(Environment* env,
                                         const char* main_script_id);
v8::MaybeLocal<v8::Object> GetPerContextExports(v8::Local<v8::Context> context);

namespace profiler {
void StartCoverageCollection(Environment* env);
}
#ifdef _WIN32
typedef SYSTEMTIME TIME_TYPE;
#else  // UNIX, OSX
typedef struct tm TIME_TYPE;
#endif

class DiagnosticFilename {
 public:
  static void LocalTime(TIME_TYPE* tm_struct);

  DiagnosticFilename(Environment* env,
                     const char* prefix,
                     const char* ext,
                     int seq = -1) :
      filename_(MakeFilename(env->thread_id(), prefix, ext, seq)) {}

  DiagnosticFilename(uint64_t thread_id,
                     const char* prefix,
                     const char* ext,
                     int seq = -1) :
      filename_(MakeFilename(thread_id, prefix, ext, seq)) {}

  const char* operator*() const { return filename_.c_str(); }

 private:
  static std::string MakeFilename(
      uint64_t thread_id,
      const char* prefix,
      const char* ext,
      int seq = -1);

  std::string filename_;
};

class TraceEventScope {
 public:
  TraceEventScope(const char* category,
                  const char* name,
                  void* id) : category_(category), name_(name), id_(id) {
    TRACE_EVENT_NESTABLE_ASYNC_BEGIN0(category_, name_, id_);
  }
  ~TraceEventScope() {
    TRACE_EVENT_NESTABLE_ASYNC_END0(category_, name_, id_);
  }

 private:
  const char* category_;
  const char* name_;
  void* id_;
};

}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#endif  // SRC_NODE_INTERNALS_H_
