/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "debugger.h"

#include <sys/uio.h>

#include <set>

#include "class_linker.h"
#include "class_loader.h"
#include "dex_instruction.h"
#if !defined(ART_USE_LLVM_COMPILER)
#include "oat/runtime/context.h"  // For VmapTable
#endif
#include "object_utils.h"
#include "safe_map.h"
#include "scoped_thread_list_lock.h"
#include "ScopedLocalRef.h"
#include "ScopedPrimitiveArray.h"
#include "space.h"
#include "stack_indirect_reference_table.h"
#include "thread_list.h"
#include "well_known_classes.h"

namespace art {

static const size_t kMaxAllocRecordStackDepth = 16; // Max 255.
static const size_t kNumAllocRecords = 512; // Must be power of 2.

static const uintptr_t kInvalidId = 1;
static const Object* kInvalidObject = reinterpret_cast<Object*>(kInvalidId);

class ObjectRegistry {
 public:
  ObjectRegistry() : lock_("ObjectRegistry lock") {
  }

  JDWP::ObjectId Add(Object* o) {
    if (o == NULL) {
      return 0;
    }
    JDWP::ObjectId id = static_cast<JDWP::ObjectId>(reinterpret_cast<uintptr_t>(o));
    MutexLock mu(lock_);
    map_.Overwrite(id, o);
    return id;
  }

  void Clear() {
    MutexLock mu(lock_);
    LOG(DEBUG) << "Debugger has detached; object registry had " << map_.size() << " entries";
    map_.clear();
  }

  bool Contains(JDWP::ObjectId id) {
    MutexLock mu(lock_);
    return map_.find(id) != map_.end();
  }

  template<typename T> T Get(JDWP::ObjectId id) {
    if (id == 0) {
      return NULL;
    }

    MutexLock mu(lock_);
    typedef SafeMap<JDWP::ObjectId, Object*>::iterator It; // C++0x auto
    It it = map_.find(id);
    return (it != map_.end()) ? reinterpret_cast<T>(it->second) : reinterpret_cast<T>(kInvalidId);
  }

  void VisitRoots(Heap::RootVisitor* visitor, void* arg) {
    MutexLock mu(lock_);
    typedef SafeMap<JDWP::ObjectId, Object*>::iterator It; // C++0x auto
    for (It it = map_.begin(); it != map_.end(); ++it) {
      visitor(it->second, arg);
    }
  }

 private:
  Mutex lock_;
  SafeMap<JDWP::ObjectId, Object*> map_;
};

struct AllocRecordStackTraceElement {
  Method* method;
  uintptr_t raw_pc;

  int32_t LineNumber() const {
    return MethodHelper(method).GetLineNumFromNativePC(raw_pc);
  }
};

struct AllocRecord {
  Class* type;
  size_t byte_count;
  uint16_t thin_lock_id;
  AllocRecordStackTraceElement stack[kMaxAllocRecordStackDepth]; // Unused entries have NULL method.

  size_t GetDepth() {
    size_t depth = 0;
    while (depth < kMaxAllocRecordStackDepth && stack[depth].method != NULL) {
      ++depth;
    }
    return depth;
  }
};

struct Breakpoint {
  Method* method;
  uint32_t dex_pc;
  Breakpoint(Method* method, uint32_t dex_pc) : method(method), dex_pc(dex_pc) {}
};

static std::ostream& operator<<(std::ostream& os, const Breakpoint& rhs) {
  os << StringPrintf("Breakpoint[%s @%#x]", PrettyMethod(rhs.method).c_str(), rhs.dex_pc);
  return os;
}

struct SingleStepControl {
  // Are we single-stepping right now?
  bool is_active;
  Thread* thread;

  JDWP::JdwpStepSize step_size;
  JDWP::JdwpStepDepth step_depth;

  const Method* method;
  int32_t line_number; // Or -1 for native methods.
  std::set<uint32_t> dex_pcs;
  int stack_depth;
};

// JDWP is allowed unless the Zygote forbids it.
static bool gJdwpAllowed = true;

// Was there a -Xrunjdwp or -agentlib:jdwp= argument on the command line?
static bool gJdwpConfigured = false;

// Broken-down JDWP options. (Only valid if IsJdwpConfigured() is true.)
static JDWP::JdwpOptions gJdwpOptions;

// Runtime JDWP state.
static JDWP::JdwpState* gJdwpState = NULL;
static bool gDebuggerConnected;  // debugger or DDMS is connected.
static bool gDebuggerActive;     // debugger is making requests.
static bool gDisposed;           // debugger called VirtualMachine.Dispose, so we should drop the connection.

static bool gDdmThreadNotification = false;

// DDMS GC-related settings.
static Dbg::HpifWhen gDdmHpifWhen = Dbg::HPIF_WHEN_NEVER;
static Dbg::HpsgWhen gDdmHpsgWhen = Dbg::HPSG_WHEN_NEVER;
static Dbg::HpsgWhat gDdmHpsgWhat;
static Dbg::HpsgWhen gDdmNhsgWhen = Dbg::HPSG_WHEN_NEVER;
static Dbg::HpsgWhat gDdmNhsgWhat;

static ObjectRegistry* gRegistry = NULL;

// Recent allocation tracking.
static Mutex gAllocTrackerLock("AllocTracker lock");
AllocRecord* Dbg::recent_allocation_records_ = NULL; // TODO: CircularBuffer<AllocRecord>
static size_t gAllocRecordHead = 0;
static size_t gAllocRecordCount = 0;

// Breakpoints and single-stepping.
static Mutex gBreakpointsLock("breakpoints lock");
static std::vector<Breakpoint> gBreakpoints;
static SingleStepControl gSingleStepControl;

static bool IsBreakpoint(Method* m, uint32_t dex_pc) {
  MutexLock mu(gBreakpointsLock);
  for (size_t i = 0; i < gBreakpoints.size(); ++i) {
    if (gBreakpoints[i].method == m && gBreakpoints[i].dex_pc == dex_pc) {
      VLOG(jdwp) << "Hit breakpoint #" << i << ": " << gBreakpoints[i];
      return true;
    }
  }
  return false;
}

static Array* DecodeArray(JDWP::RefTypeId id, JDWP::JdwpError& status) {
  Object* o = gRegistry->Get<Object*>(id);
  if (o == NULL || o == kInvalidObject) {
    status = JDWP::ERR_INVALID_OBJECT;
    return NULL;
  }
  if (!o->IsArrayInstance()) {
    status = JDWP::ERR_INVALID_ARRAY;
    return NULL;
  }
  status = JDWP::ERR_NONE;
  return o->AsArray();
}

static Class* DecodeClass(JDWP::RefTypeId id, JDWP::JdwpError& status) {
  Object* o = gRegistry->Get<Object*>(id);
  if (o == NULL || o == kInvalidObject) {
    status = JDWP::ERR_INVALID_OBJECT;
    return NULL;
  }
  if (!o->IsClass()) {
    status = JDWP::ERR_INVALID_CLASS;
    return NULL;
  }
  status = JDWP::ERR_NONE;
  return o->AsClass();
}

static Thread* DecodeThread(JDWP::ObjectId threadId) {
  Object* thread_peer = gRegistry->Get<Object*>(threadId);
  if (thread_peer == NULL || thread_peer == kInvalidObject) {
    return NULL;
  }
  return Thread::FromManagedThread(thread_peer);
}

static JDWP::JdwpTag BasicTagFromDescriptor(const char* descriptor) {
  // JDWP deliberately uses the descriptor characters' ASCII values for its enum.
  // Note that by "basic" we mean that we don't get more specific than JT_OBJECT.
  return static_cast<JDWP::JdwpTag>(descriptor[0]);
}

static JDWP::JdwpTag TagFromClass(Class* c) {
  CHECK(c != NULL);
  if (c->IsArrayClass()) {
    return JDWP::JT_ARRAY;
  }

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  if (c->IsStringClass()) {
    return JDWP::JT_STRING;
  } else if (c->IsClassClass()) {
    return JDWP::JT_CLASS_OBJECT;
  } else if (class_linker->FindSystemClass("Ljava/lang/Thread;")->IsAssignableFrom(c)) {
    return JDWP::JT_THREAD;
  } else if (class_linker->FindSystemClass("Ljava/lang/ThreadGroup;")->IsAssignableFrom(c)) {
    return JDWP::JT_THREAD_GROUP;
  } else if (class_linker->FindSystemClass("Ljava/lang/ClassLoader;")->IsAssignableFrom(c)) {
    return JDWP::JT_CLASS_LOADER;
  } else {
    return JDWP::JT_OBJECT;
  }
}

/*
 * Objects declared to hold Object might actually hold a more specific
 * type.  The debugger may take a special interest in these (e.g. it
 * wants to display the contents of Strings), so we want to return an
 * appropriate tag.
 *
 * Null objects are tagged JT_OBJECT.
 */
static JDWP::JdwpTag TagFromObject(const Object* o) {
  return (o == NULL) ? JDWP::JT_OBJECT : TagFromClass(o->GetClass());
}

static bool IsPrimitiveTag(JDWP::JdwpTag tag) {
  switch (tag) {
  case JDWP::JT_BOOLEAN:
  case JDWP::JT_BYTE:
  case JDWP::JT_CHAR:
  case JDWP::JT_FLOAT:
  case JDWP::JT_DOUBLE:
  case JDWP::JT_INT:
  case JDWP::JT_LONG:
  case JDWP::JT_SHORT:
  case JDWP::JT_VOID:
    return true;
  default:
    return false;
  }
}

/*
 * Handle one of the JDWP name/value pairs.
 *
 * JDWP options are:
 *  help: if specified, show help message and bail
 *  transport: may be dt_socket or dt_shmem
 *  address: for dt_socket, "host:port", or just "port" when listening
 *  server: if "y", wait for debugger to attach; if "n", attach to debugger
 *  timeout: how long to wait for debugger to connect / listen
 *
 * Useful with server=n (these aren't supported yet):
 *  onthrow=<exception-name>: connect to debugger when exception thrown
 *  onuncaught=y|n: connect to debugger when uncaught exception thrown
 *  launch=<command-line>: launch the debugger itself
 *
 * The "transport" option is required, as is "address" if server=n.
 */
static bool ParseJdwpOption(const std::string& name, const std::string& value) {
  if (name == "transport") {
    if (value == "dt_socket") {
      gJdwpOptions.transport = JDWP::kJdwpTransportSocket;
    } else if (value == "dt_android_adb") {
      gJdwpOptions.transport = JDWP::kJdwpTransportAndroidAdb;
    } else {
      LOG(ERROR) << "JDWP transport not supported: " << value;
      return false;
    }
  } else if (name == "server") {
    if (value == "n") {
      gJdwpOptions.server = false;
    } else if (value == "y") {
      gJdwpOptions.server = true;
    } else {
      LOG(ERROR) << "JDWP option 'server' must be 'y' or 'n'";
      return false;
    }
  } else if (name == "suspend") {
    if (value == "n") {
      gJdwpOptions.suspend = false;
    } else if (value == "y") {
      gJdwpOptions.suspend = true;
    } else {
      LOG(ERROR) << "JDWP option 'suspend' must be 'y' or 'n'";
      return false;
    }
  } else if (name == "address") {
    /* this is either <port> or <host>:<port> */
    std::string port_string;
    gJdwpOptions.host.clear();
    std::string::size_type colon = value.find(':');
    if (colon != std::string::npos) {
      gJdwpOptions.host = value.substr(0, colon);
      port_string = value.substr(colon + 1);
    } else {
      port_string = value;
    }
    if (port_string.empty()) {
      LOG(ERROR) << "JDWP address missing port: " << value;
      return false;
    }
    char* end;
    uint64_t port = strtoul(port_string.c_str(), &end, 10);
    if (*end != '\0' || port > 0xffff) {
      LOG(ERROR) << "JDWP address has junk in port field: " << value;
      return false;
    }
    gJdwpOptions.port = port;
  } else if (name == "launch" || name == "onthrow" || name == "oncaught" || name == "timeout") {
    /* valid but unsupported */
    LOG(INFO) << "Ignoring JDWP option '" << name << "'='" << value << "'";
  } else {
    LOG(INFO) << "Ignoring unrecognized JDWP option '" << name << "'='" << value << "'";
  }

  return true;
}

/*
 * Parse the latter half of a -Xrunjdwp/-agentlib:jdwp= string, e.g.:
 * "transport=dt_socket,address=8000,server=y,suspend=n"
 */
bool Dbg::ParseJdwpOptions(const std::string& options) {
  VLOG(jdwp) << "ParseJdwpOptions: " << options;

  std::vector<std::string> pairs;
  Split(options, ',', pairs);

  for (size_t i = 0; i < pairs.size(); ++i) {
    std::string::size_type equals = pairs[i].find('=');
    if (equals == std::string::npos) {
      LOG(ERROR) << "Can't parse JDWP option '" << pairs[i] << "' in '" << options << "'";
      return false;
    }
    ParseJdwpOption(pairs[i].substr(0, equals), pairs[i].substr(equals + 1));
  }

  if (gJdwpOptions.transport == JDWP::kJdwpTransportUnknown) {
    LOG(ERROR) << "Must specify JDWP transport: " << options;
  }
  if (!gJdwpOptions.server && (gJdwpOptions.host.empty() || gJdwpOptions.port == 0)) {
    LOG(ERROR) << "Must specify JDWP host and port when server=n: " << options;
    return false;
  }

  gJdwpConfigured = true;
  return true;
}

void Dbg::StartJdwp() {
  if (!gJdwpAllowed || !IsJdwpConfigured()) {
    // No JDWP for you!
    return;
  }

  CHECK(gRegistry == NULL);
  gRegistry = new ObjectRegistry;

  // Init JDWP if the debugger is enabled. This may connect out to a
  // debugger, passively listen for a debugger, or block waiting for a
  // debugger.
  gJdwpState = JDWP::JdwpState::Create(&gJdwpOptions);
  if (gJdwpState == NULL) {
    // We probably failed because some other process has the port already, which means that
    // if we don't abort the user is likely to think they're talking to us when they're actually
    // talking to that other process.
    LOG(FATAL) << "Debugger thread failed to initialize";
  }

  // If a debugger has already attached, send the "welcome" message.
  // This may cause us to suspend all threads.
  if (gJdwpState->IsActive()) {
    //ScopedThreadStateChange tsc(Thread::Current(), kRunnable);
    if (!gJdwpState->PostVMStart()) {
      LOG(WARNING) << "Failed to post 'start' message to debugger";
    }
  }
}

void Dbg::StopJdwp() {
  delete gJdwpState;
  delete gRegistry;
  gRegistry = NULL;
}

void Dbg::GcDidFinish() {
  if (gDdmHpifWhen != HPIF_WHEN_NEVER) {
    LOG(DEBUG) << "Sending heap info to DDM";
    DdmSendHeapInfo(gDdmHpifWhen);
  }
  if (gDdmHpsgWhen != HPSG_WHEN_NEVER) {
    LOG(DEBUG) << "Dumping heap to DDM";
    DdmSendHeapSegments(false);
  }
  if (gDdmNhsgWhen != HPSG_WHEN_NEVER) {
    LOG(DEBUG) << "Dumping native heap to DDM";
    DdmSendHeapSegments(true);
  }
}

void Dbg::SetJdwpAllowed(bool allowed) {
  gJdwpAllowed = allowed;
}

DebugInvokeReq* Dbg::GetInvokeReq() {
  return Thread::Current()->GetInvokeReq();
}

Thread* Dbg::GetDebugThread() {
  return (gJdwpState != NULL) ? gJdwpState->GetDebugThread() : NULL;
}

void Dbg::ClearWaitForEventThread() {
  gJdwpState->ClearWaitForEventThread();
}

void Dbg::Connected() {
  CHECK(!gDebuggerConnected);
  VLOG(jdwp) << "JDWP has attached";
  gDebuggerConnected = true;
  gDisposed = false;
}

void Dbg::Disposed() {
  gDisposed = true;
}

bool Dbg::IsDisposed() {
  return gDisposed;
}

static void SetDebuggerUpdatesEnabledCallback(Thread* t, void* user_data) {
  t->SetDebuggerUpdatesEnabled(*reinterpret_cast<bool*>(user_data));
}

static void SetDebuggerUpdatesEnabled(bool enabled) {
  Runtime* runtime = Runtime::Current();
  ScopedThreadListLock thread_list_lock;
  runtime->GetThreadList()->ForEach(SetDebuggerUpdatesEnabledCallback, &enabled);
}

void Dbg::GoActive() {
  // Enable all debugging features, including scans for breakpoints.
  // This is a no-op if we're already active.
  // Only called from the JDWP handler thread.
  if (gDebuggerActive) {
    return;
  }

  LOG(INFO) << "Debugger is active";

  {
    // TODO: dalvik only warned if there were breakpoints left over. clear in Dbg::Disconnected?
    MutexLock mu(gBreakpointsLock);
    CHECK_EQ(gBreakpoints.size(), 0U);
  }

  gDebuggerActive = true;
  SetDebuggerUpdatesEnabled(true);
}

void Dbg::Disconnected() {
  CHECK(gDebuggerConnected);

  LOG(INFO) << "Debugger is no longer active";

  gDebuggerActive = false;
  SetDebuggerUpdatesEnabled(false);

  gRegistry->Clear();
  gDebuggerConnected = false;
}

bool Dbg::IsDebuggerActive() {
  return gDebuggerActive;
}

bool Dbg::IsJdwpConfigured() {
  return gJdwpConfigured;
}

int64_t Dbg::LastDebuggerActivity() {
  return gJdwpState->LastDebuggerActivity();
}

int Dbg::ThreadRunning() {
  return static_cast<int>(Thread::Current()->SetState(kRunnable));
}

int Dbg::ThreadWaiting() {
  return static_cast<int>(Thread::Current()->SetState(kVmWait));
}

int Dbg::ThreadContinuing(int new_state) {
  return static_cast<int>(Thread::Current()->SetState(static_cast<ThreadState>(new_state)));
}

void Dbg::UndoDebuggerSuspensions() {
  Runtime::Current()->GetThreadList()->UndoDebuggerSuspensions();
}

void Dbg::Exit(int status) {
  exit(status); // This is all dalvik did.
}

void Dbg::VisitRoots(Heap::RootVisitor* visitor, void* arg) {
  if (gRegistry != NULL) {
    gRegistry->VisitRoots(visitor, arg);
  }
}

std::string Dbg::GetClassName(JDWP::RefTypeId classId) {
  Object* o = gRegistry->Get<Object*>(classId);
  if (o == NULL) {
    return "NULL";
  }
  if (o == kInvalidObject) {
    return StringPrintf("invalid object %p", reinterpret_cast<void*>(classId));
  }
  if (!o->IsClass()) {
    return StringPrintf("non-class %p", o); // This is only used for debugging output anyway.
  }
  return DescriptorToName(ClassHelper(o->AsClass()).GetDescriptor());
}

JDWP::JdwpError Dbg::GetClassObject(JDWP::RefTypeId id, JDWP::ObjectId& classObjectId) {
  JDWP::JdwpError status;
  Class* c = DecodeClass(id, status);
  if (c == NULL) {
    return status;
  }
  classObjectId = gRegistry->Add(c);
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetSuperclass(JDWP::RefTypeId id, JDWP::RefTypeId& superclassId) {
  JDWP::JdwpError status;
  Class* c = DecodeClass(id, status);
  if (c == NULL) {
    return status;
  }
  if (c->IsInterface()) {
    // http://code.google.com/p/android/issues/detail?id=20856
    superclassId = 0;
  } else {
    superclassId = gRegistry->Add(c->GetSuperClass());
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetClassLoader(JDWP::RefTypeId id, JDWP::ExpandBuf* pReply) {
  Object* o = gRegistry->Get<Object*>(id);
  if (o == NULL || o == kInvalidObject) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  expandBufAddObjectId(pReply, gRegistry->Add(o->GetClass()->GetClassLoader()));
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetModifiers(JDWP::RefTypeId id, JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError status;
  Class* c = DecodeClass(id, status);
  if (c == NULL) {
    return status;
  }

  uint32_t access_flags = c->GetAccessFlags() & kAccJavaFlagsMask;

  // Set ACC_SUPER; dex files don't contain this flag, but all classes are supposed to have it set.
  // Class.getModifiers doesn't return it, but JDWP does, so we set it here.
  access_flags |= kAccSuper;

  expandBufAdd4BE(pReply, access_flags);

  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetReflectedType(JDWP::RefTypeId classId, JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError status;
  Class* c = DecodeClass(classId, status);
  if (c == NULL) {
    return status;
  }

  expandBufAdd1(pReply, c->IsInterface() ? JDWP::TT_INTERFACE : JDWP::TT_CLASS);
  expandBufAddRefTypeId(pReply, classId);
  return JDWP::ERR_NONE;
}

void Dbg::GetClassList(std::vector<JDWP::RefTypeId>& classes) {
  // Get the complete list of reference classes (i.e. all classes except
  // the primitive types).
  // Returns a newly-allocated buffer full of RefTypeId values.
  struct ClassListCreator {
    explicit ClassListCreator(std::vector<JDWP::RefTypeId>& classes) : classes(classes) {
    }

    static bool Visit(Class* c, void* arg) {
      return reinterpret_cast<ClassListCreator*>(arg)->Visit(c);
    }

    bool Visit(Class* c) {
      if (!c->IsPrimitive()) {
        classes.push_back(static_cast<JDWP::RefTypeId>(gRegistry->Add(c)));
      }
      return true;
    }

    std::vector<JDWP::RefTypeId>& classes;
  };

  ClassListCreator clc(classes);
  Runtime::Current()->GetClassLinker()->VisitClasses(ClassListCreator::Visit, &clc);
}

JDWP::JdwpError Dbg::GetClassInfo(JDWP::RefTypeId classId, JDWP::JdwpTypeTag* pTypeTag, uint32_t* pStatus, std::string* pDescriptor) {
  JDWP::JdwpError status;
  Class* c = DecodeClass(classId, status);
  if (c == NULL) {
    return status;
  }

  if (c->IsArrayClass()) {
    *pStatus = JDWP::CS_VERIFIED | JDWP::CS_PREPARED;
    *pTypeTag = JDWP::TT_ARRAY;
  } else {
    if (c->IsErroneous()) {
      *pStatus = JDWP::CS_ERROR;
    } else {
      *pStatus = JDWP::CS_VERIFIED | JDWP::CS_PREPARED | JDWP::CS_INITIALIZED;
    }
    *pTypeTag = c->IsInterface() ? JDWP::TT_INTERFACE : JDWP::TT_CLASS;
  }

  if (pDescriptor != NULL) {
    *pDescriptor = ClassHelper(c).GetDescriptor();
  }
  return JDWP::ERR_NONE;
}

void Dbg::FindLoadedClassBySignature(const char* descriptor, std::vector<JDWP::RefTypeId>& ids) {
  std::vector<Class*> classes;
  Runtime::Current()->GetClassLinker()->LookupClasses(descriptor, classes);
  ids.clear();
  for (size_t i = 0; i < classes.size(); ++i) {
    ids.push_back(gRegistry->Add(classes[i]));
  }
}

JDWP::JdwpError Dbg::GetReferenceType(JDWP::ObjectId objectId, JDWP::ExpandBuf* pReply) {
  Object* o = gRegistry->Get<Object*>(objectId);
  if (o == NULL || o == kInvalidObject) {
    return JDWP::ERR_INVALID_OBJECT;
  }

  JDWP::JdwpTypeTag type_tag;
  if (o->GetClass()->IsArrayClass()) {
    type_tag = JDWP::TT_ARRAY;
  } else if (o->GetClass()->IsInterface()) {
    type_tag = JDWP::TT_INTERFACE;
  } else {
    type_tag = JDWP::TT_CLASS;
  }
  JDWP::RefTypeId type_id = gRegistry->Add(o->GetClass());

  expandBufAdd1(pReply, type_tag);
  expandBufAddRefTypeId(pReply, type_id);

  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetSignature(JDWP::RefTypeId classId, std::string& signature) {
  JDWP::JdwpError status;
  Class* c = DecodeClass(classId, status);
  if (c == NULL) {
    return status;
  }
  signature = ClassHelper(c).GetDescriptor();
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetSourceFile(JDWP::RefTypeId classId, std::string& result) {
  JDWP::JdwpError status;
  Class* c = DecodeClass(classId, status);
  if (c == NULL) {
    return status;
  }
  result = ClassHelper(c).GetSourceFile();
  return JDWP::ERR_NONE;
}

uint8_t Dbg::GetObjectTag(JDWP::ObjectId objectId) {
  Object* o = gRegistry->Get<Object*>(objectId);
  return TagFromObject(o);
}

size_t Dbg::GetTagWidth(JDWP::JdwpTag tag) {
  switch (tag) {
  case JDWP::JT_VOID:
    return 0;
  case JDWP::JT_BYTE:
  case JDWP::JT_BOOLEAN:
    return 1;
  case JDWP::JT_CHAR:
  case JDWP::JT_SHORT:
    return 2;
  case JDWP::JT_FLOAT:
  case JDWP::JT_INT:
    return 4;
  case JDWP::JT_ARRAY:
  case JDWP::JT_OBJECT:
  case JDWP::JT_STRING:
  case JDWP::JT_THREAD:
  case JDWP::JT_THREAD_GROUP:
  case JDWP::JT_CLASS_LOADER:
  case JDWP::JT_CLASS_OBJECT:
    return sizeof(JDWP::ObjectId);
  case JDWP::JT_DOUBLE:
  case JDWP::JT_LONG:
    return 8;
  default:
    LOG(FATAL) << "Unknown tag " << tag;
    return -1;
  }
}

JDWP::JdwpError Dbg::GetArrayLength(JDWP::ObjectId arrayId, int& length) {
  JDWP::JdwpError status;
  Array* a = DecodeArray(arrayId, status);
  if (a == NULL) {
    return status;
  }
  length = a->GetLength();
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::OutputArray(JDWP::ObjectId arrayId, int offset, int count, JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError status;
  Array* a = DecodeArray(arrayId, status);
  if (a == NULL) {
    return status;
  }

  if (offset < 0 || count < 0 || offset > a->GetLength() || a->GetLength() - offset < count) {
    LOG(WARNING) << __FUNCTION__ << " access out of bounds: offset=" << offset << "; count=" << count;
    return JDWP::ERR_INVALID_LENGTH;
  }
  std::string descriptor(ClassHelper(a->GetClass()).GetDescriptor());
  JDWP::JdwpTag tag = BasicTagFromDescriptor(descriptor.c_str() + 1);

  expandBufAdd1(pReply, tag);
  expandBufAdd4BE(pReply, count);

  if (IsPrimitiveTag(tag)) {
    size_t width = GetTagWidth(tag);
    uint8_t* dst = expandBufAddSpace(pReply, count * width);
    if (width == 8) {
      const uint64_t* src8 = reinterpret_cast<uint64_t*>(a->GetRawData(sizeof(uint64_t)));
      for (int i = 0; i < count; ++i) JDWP::Write8BE(&dst, src8[offset + i]);
    } else if (width == 4) {
      const uint32_t* src4 = reinterpret_cast<uint32_t*>(a->GetRawData(sizeof(uint32_t)));
      for (int i = 0; i < count; ++i) JDWP::Write4BE(&dst, src4[offset + i]);
    } else if (width == 2) {
      const uint16_t* src2 = reinterpret_cast<uint16_t*>(a->GetRawData(sizeof(uint16_t)));
      for (int i = 0; i < count; ++i) JDWP::Write2BE(&dst, src2[offset + i]);
    } else {
      const uint8_t* src = reinterpret_cast<uint8_t*>(a->GetRawData(sizeof(uint8_t)));
      memcpy(dst, &src[offset * width], count * width);
    }
  } else {
    ObjectArray<Object>* oa = a->AsObjectArray<Object>();
    for (int i = 0; i < count; ++i) {
      Object* element = oa->Get(offset + i);
      JDWP::JdwpTag specific_tag = (element != NULL) ? TagFromObject(element) : tag;
      expandBufAdd1(pReply, specific_tag);
      expandBufAddObjectId(pReply, gRegistry->Add(element));
    }
  }

  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::SetArrayElements(JDWP::ObjectId arrayId, int offset, int count, const uint8_t* src) {
  JDWP::JdwpError status;
  Array* a = DecodeArray(arrayId, status);
  if (a == NULL) {
    return status;
  }

  if (offset < 0 || count < 0 || offset > a->GetLength() || a->GetLength() - offset < count) {
    LOG(WARNING) << __FUNCTION__ << " access out of bounds: offset=" << offset << "; count=" << count;
    return JDWP::ERR_INVALID_LENGTH;
  }
  std::string descriptor(ClassHelper(a->GetClass()).GetDescriptor());
  JDWP::JdwpTag tag = BasicTagFromDescriptor(descriptor.c_str() + 1);

  if (IsPrimitiveTag(tag)) {
    size_t width = GetTagWidth(tag);
    if (width == 8) {
      uint8_t* dst = &(reinterpret_cast<uint8_t*>(a->GetRawData(sizeof(uint64_t)))[offset * width]);
      for (int i = 0; i < count; ++i) {
        // Handle potentially non-aligned memory access one byte at a time for ARM's benefit.
        uint64_t value;
        for (size_t j = 0; j < sizeof(uint64_t); ++j) reinterpret_cast<uint8_t*>(&value)[j] = src[j];
        src += sizeof(uint64_t);
        JDWP::Write8BE(&dst, value);
      }
    } else if (width == 4) {
      uint8_t* dst = &(reinterpret_cast<uint8_t*>(a->GetRawData(sizeof(uint32_t)))[offset * width]);
      const uint32_t* src4 = reinterpret_cast<const uint32_t*>(src);
      for (int i = 0; i < count; ++i) JDWP::Write4BE(&dst, src4[i]);
    } else if (width == 2) {
      uint8_t* dst = &(reinterpret_cast<uint8_t*>(a->GetRawData(sizeof(uint16_t)))[offset * width]);
      const uint16_t* src2 = reinterpret_cast<const uint16_t*>(src);
      for (int i = 0; i < count; ++i) JDWP::Write2BE(&dst, src2[i]);
    } else {
      uint8_t* dst = &(reinterpret_cast<uint8_t*>(a->GetRawData(sizeof(uint8_t)))[offset * width]);
      memcpy(&dst[offset * width], src, count * width);
    }
  } else {
    ObjectArray<Object>* oa = a->AsObjectArray<Object>();
    for (int i = 0; i < count; ++i) {
      JDWP::ObjectId id = JDWP::ReadObjectId(&src);
      Object* o = gRegistry->Get<Object*>(id);
      if (o == kInvalidObject) {
        return JDWP::ERR_INVALID_OBJECT;
      }
      oa->Set(offset + i, o);
    }
  }

  return JDWP::ERR_NONE;
}

JDWP::ObjectId Dbg::CreateString(const std::string& str) {
  return gRegistry->Add(String::AllocFromModifiedUtf8(str.c_str()));
}

JDWP::JdwpError Dbg::CreateObject(JDWP::RefTypeId classId, JDWP::ObjectId& new_object) {
  JDWP::JdwpError status;
  Class* c = DecodeClass(classId, status);
  if (c == NULL) {
    return status;
  }
  new_object = gRegistry->Add(c->AllocObject());
  return JDWP::ERR_NONE;
}

/*
 * Used by Eclipse's "Display" view to evaluate "new byte[5]" to get "(byte[]) [0, 0, 0, 0, 0]".
 */
JDWP::JdwpError Dbg::CreateArrayObject(JDWP::RefTypeId arrayClassId, uint32_t length, JDWP::ObjectId& new_array) {
  JDWP::JdwpError status;
  Class* c = DecodeClass(arrayClassId, status);
  if (c == NULL) {
    return status;
  }
  new_array = gRegistry->Add(Array::Alloc(c, length));
  return JDWP::ERR_NONE;
}

bool Dbg::MatchType(JDWP::RefTypeId instClassId, JDWP::RefTypeId classId) {
  JDWP::JdwpError status;
  Class* c1 = DecodeClass(instClassId, status);
  CHECK(c1 != NULL);
  Class* c2 = DecodeClass(classId, status);
  CHECK(c2 != NULL);
  return c1->IsAssignableFrom(c2);
}

static JDWP::FieldId ToFieldId(const Field* f) {
#ifdef MOVING_GARBAGE_COLLECTOR
  UNIMPLEMENTED(FATAL);
#else
  return static_cast<JDWP::FieldId>(reinterpret_cast<uintptr_t>(f));
#endif
}

static JDWP::MethodId ToMethodId(const Method* m) {
#ifdef MOVING_GARBAGE_COLLECTOR
  UNIMPLEMENTED(FATAL);
#else
  return static_cast<JDWP::MethodId>(reinterpret_cast<uintptr_t>(m));
#endif
}

static Field* FromFieldId(JDWP::FieldId fid) {
#ifdef MOVING_GARBAGE_COLLECTOR
  UNIMPLEMENTED(FATAL);
#else
  return reinterpret_cast<Field*>(static_cast<uintptr_t>(fid));
#endif
}

static Method* FromMethodId(JDWP::MethodId mid) {
#ifdef MOVING_GARBAGE_COLLECTOR
  UNIMPLEMENTED(FATAL);
#else
  return reinterpret_cast<Method*>(static_cast<uintptr_t>(mid));
#endif
}

static void SetLocation(JDWP::JdwpLocation& location, Method* m, uintptr_t native_pc) {
  if (m == NULL) {
    memset(&location, 0, sizeof(location));
  } else {
    Class* c = m->GetDeclaringClass();
    location.typeTag = c->IsInterface() ? JDWP::TT_INTERFACE : JDWP::TT_CLASS;
    location.classId = gRegistry->Add(c);
    location.methodId = ToMethodId(m);
    location.dex_pc = m->IsNative() ? -1 : m->ToDexPC(native_pc);
  }
}

std::string Dbg::GetMethodName(JDWP::RefTypeId, JDWP::MethodId methodId) {
  Method* m = FromMethodId(methodId);
  return MethodHelper(m).GetName();
}

/*
 * Augment the access flags for synthetic methods and fields by setting
 * the (as described by the spec) "0xf0000000 bit".  Also, strip out any
 * flags not specified by the Java programming language.
 */
static uint32_t MangleAccessFlags(uint32_t accessFlags) {
  accessFlags &= kAccJavaFlagsMask;
  if ((accessFlags & kAccSynthetic) != 0) {
    accessFlags |= 0xf0000000;
  }
  return accessFlags;
}

static const uint16_t kEclipseWorkaroundSlot = 1000;

/*
 * Eclipse appears to expect that the "this" reference is in slot zero.
 * If it's not, the "variables" display will show two copies of "this",
 * possibly because it gets "this" from SF.ThisObject and then displays
 * all locals with nonzero slot numbers.
 *
 * So, we remap the item in slot 0 to 1000, and remap "this" to zero.  On
 * SF.GetValues / SF.SetValues we map them back.
 *
 * TODO: jdb uses the value to determine whether a variable is a local or an argument,
 * by checking whether it's less than the number of arguments. To make that work, we'd
 * have to "mangle" all the arguments to come first, not just the implicit argument 'this'.
 */
static uint16_t MangleSlot(uint16_t slot, const char* name) {
  uint16_t newSlot = slot;
  if (strcmp(name, "this") == 0) {
    newSlot = 0;
  } else if (slot == 0) {
    newSlot = kEclipseWorkaroundSlot;
  }
  return newSlot;
}

static uint16_t DemangleSlot(uint16_t slot, Method* m) {
  if (slot == kEclipseWorkaroundSlot) {
    return 0;
  } else if (slot == 0) {
    const DexFile::CodeItem* code_item = MethodHelper(m).GetCodeItem();
    CHECK(code_item != NULL);
    return code_item->registers_size_ - code_item->ins_size_;
  }
  return slot;
}

JDWP::JdwpError Dbg::OutputDeclaredFields(JDWP::RefTypeId classId, bool with_generic, JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError status;
  Class* c = DecodeClass(classId, status);
  if (c == NULL) {
    return status;
  }

  size_t instance_field_count = c->NumInstanceFields();
  size_t static_field_count = c->NumStaticFields();

  expandBufAdd4BE(pReply, instance_field_count + static_field_count);

  for (size_t i = 0; i < instance_field_count + static_field_count; ++i) {
    Field* f = (i < instance_field_count) ? c->GetInstanceField(i) : c->GetStaticField(i - instance_field_count);
    FieldHelper fh(f);
    expandBufAddFieldId(pReply, ToFieldId(f));
    expandBufAddUtf8String(pReply, fh.GetName());
    expandBufAddUtf8String(pReply, fh.GetTypeDescriptor());
    if (with_generic) {
      static const char genericSignature[1] = "";
      expandBufAddUtf8String(pReply, genericSignature);
    }
    expandBufAdd4BE(pReply, MangleAccessFlags(f->GetAccessFlags()));
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::OutputDeclaredMethods(JDWP::RefTypeId classId, bool with_generic, JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError status;
  Class* c = DecodeClass(classId, status);
  if (c == NULL) {
    return status;
  }

  size_t direct_method_count = c->NumDirectMethods();
  size_t virtual_method_count = c->NumVirtualMethods();

  expandBufAdd4BE(pReply, direct_method_count + virtual_method_count);

  for (size_t i = 0; i < direct_method_count + virtual_method_count; ++i) {
    Method* m = (i < direct_method_count) ? c->GetDirectMethod(i) : c->GetVirtualMethod(i - direct_method_count);
    MethodHelper mh(m);
    expandBufAddMethodId(pReply, ToMethodId(m));
    expandBufAddUtf8String(pReply, mh.GetName());
    expandBufAddUtf8String(pReply, mh.GetSignature());
    if (with_generic) {
      static const char genericSignature[1] = "";
      expandBufAddUtf8String(pReply, genericSignature);
    }
    expandBufAdd4BE(pReply, MangleAccessFlags(m->GetAccessFlags()));
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::OutputDeclaredInterfaces(JDWP::RefTypeId classId, JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError status;
  Class* c = DecodeClass(classId, status);
  if (c == NULL) {
    return status;
  }

  ClassHelper kh(c);
  size_t interface_count = kh.NumDirectInterfaces();
  expandBufAdd4BE(pReply, interface_count);
  for (size_t i = 0; i < interface_count; ++i) {
    expandBufAddRefTypeId(pReply, gRegistry->Add(kh.GetDirectInterface(i)));
  }
  return JDWP::ERR_NONE;
}

void Dbg::OutputLineTable(JDWP::RefTypeId, JDWP::MethodId methodId, JDWP::ExpandBuf* pReply) {
  struct DebugCallbackContext {
    int numItems;
    JDWP::ExpandBuf* pReply;

    static bool Callback(void* context, uint32_t address, uint32_t line_number) {
      DebugCallbackContext* pContext = reinterpret_cast<DebugCallbackContext*>(context);
      expandBufAdd8BE(pContext->pReply, address);
      expandBufAdd4BE(pContext->pReply, line_number);
      pContext->numItems++;
      return true;
    }
  };

  Method* m = FromMethodId(methodId);
  MethodHelper mh(m);
  uint64_t start, end;
  if (m->IsNative()) {
    start = -1;
    end = -1;
  } else {
    start = 0;
    // TODO: what are the units supposed to be? *2?
    end = mh.GetCodeItem()->insns_size_in_code_units_;
  }

  expandBufAdd8BE(pReply, start);
  expandBufAdd8BE(pReply, end);

  // Add numLines later
  size_t numLinesOffset = expandBufGetLength(pReply);
  expandBufAdd4BE(pReply, 0);

  DebugCallbackContext context;
  context.numItems = 0;
  context.pReply = pReply;

  mh.GetDexFile().DecodeDebugInfo(mh.GetCodeItem(), m->IsStatic(), m->GetDexMethodIndex(),
                                  DebugCallbackContext::Callback, NULL, &context);

  JDWP::Set4BE(expandBufGetBuffer(pReply) + numLinesOffset, context.numItems);
}

void Dbg::OutputVariableTable(JDWP::RefTypeId, JDWP::MethodId methodId, bool with_generic, JDWP::ExpandBuf* pReply) {
  struct DebugCallbackContext {
    JDWP::ExpandBuf* pReply;
    size_t variable_count;
    bool with_generic;

    static void Callback(void* context, uint16_t slot, uint32_t startAddress, uint32_t endAddress, const char* name, const char* descriptor, const char* signature) {
      DebugCallbackContext* pContext = reinterpret_cast<DebugCallbackContext*>(context);

      VLOG(jdwp) << StringPrintf("    %2zd: %d(%d) '%s' '%s' '%s' actual slot=%d mangled slot=%d", pContext->variable_count, startAddress, endAddress - startAddress, name, descriptor, signature, slot, MangleSlot(slot, name));

      slot = MangleSlot(slot, name);

      expandBufAdd8BE(pContext->pReply, startAddress);
      expandBufAddUtf8String(pContext->pReply, name);
      expandBufAddUtf8String(pContext->pReply, descriptor);
      if (pContext->with_generic) {
        expandBufAddUtf8String(pContext->pReply, signature);
      }
      expandBufAdd4BE(pContext->pReply, endAddress - startAddress);
      expandBufAdd4BE(pContext->pReply, slot);

      ++pContext->variable_count;
    }
  };

  Method* m = FromMethodId(methodId);
  MethodHelper mh(m);
  const DexFile::CodeItem* code_item = mh.GetCodeItem();

  // arg_count considers doubles and longs to take 2 units.
  // variable_count considers everything to take 1 unit.
  std::string shorty(mh.GetShorty());
  expandBufAdd4BE(pReply, m->NumArgRegisters(shorty));

  // We don't know the total number of variables yet, so leave a blank and update it later.
  size_t variable_count_offset = expandBufGetLength(pReply);
  expandBufAdd4BE(pReply, 0);

  DebugCallbackContext context;
  context.pReply = pReply;
  context.variable_count = 0;
  context.with_generic = with_generic;

  mh.GetDexFile().DecodeDebugInfo(code_item, m->IsStatic(), m->GetDexMethodIndex(), NULL,
                                  DebugCallbackContext::Callback, &context);

  JDWP::Set4BE(expandBufGetBuffer(pReply) + variable_count_offset, context.variable_count);
}

JDWP::JdwpTag Dbg::GetFieldBasicTag(JDWP::FieldId fieldId) {
  return BasicTagFromDescriptor(FieldHelper(FromFieldId(fieldId)).GetTypeDescriptor());
}

JDWP::JdwpTag Dbg::GetStaticFieldBasicTag(JDWP::FieldId fieldId) {
  return BasicTagFromDescriptor(FieldHelper(FromFieldId(fieldId)).GetTypeDescriptor());
}

static JDWP::JdwpError GetFieldValueImpl(JDWP::RefTypeId refTypeId, JDWP::ObjectId objectId, JDWP::FieldId fieldId, JDWP::ExpandBuf* pReply, bool is_static) {
  JDWP::JdwpError status;
  Class* c = DecodeClass(refTypeId, status);
  if (refTypeId != 0 && c == NULL) {
    return status;
  }

  Object* o = gRegistry->Get<Object*>(objectId);
  if ((!is_static && o == NULL) || o == kInvalidObject) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  Field* f = FromFieldId(fieldId);

  Class* receiver_class = c;
  if (receiver_class == NULL && o != NULL) {
    receiver_class = o->GetClass();
  }
  // TODO: should we give up now if receiver_class is NULL?
  if (receiver_class != NULL && !f->GetDeclaringClass()->IsAssignableFrom(receiver_class)) {
    LOG(INFO) << "ERR_INVALID_FIELDID: " << PrettyField(f) << " " << PrettyClass(receiver_class);
    return JDWP::ERR_INVALID_FIELDID;
  }

  // The RI only enforces the static/non-static mismatch in one direction.
  // TODO: should we change the tests and check both?
  if (is_static) {
    if (!f->IsStatic()) {
      return JDWP::ERR_INVALID_FIELDID;
    }
  } else {
    if (f->IsStatic()) {
      LOG(WARNING) << "Ignoring non-NULL receiver for ObjectReference.SetValues on static field " << PrettyField(f);
      o = NULL;
    }
  }

  JDWP::JdwpTag tag = BasicTagFromDescriptor(FieldHelper(f).GetTypeDescriptor());

  if (IsPrimitiveTag(tag)) {
    expandBufAdd1(pReply, tag);
    if (tag == JDWP::JT_BOOLEAN || tag == JDWP::JT_BYTE) {
      expandBufAdd1(pReply, f->Get32(o));
    } else if (tag == JDWP::JT_CHAR || tag == JDWP::JT_SHORT) {
      expandBufAdd2BE(pReply, f->Get32(o));
    } else if (tag == JDWP::JT_FLOAT || tag == JDWP::JT_INT) {
      expandBufAdd4BE(pReply, f->Get32(o));
    } else if (tag == JDWP::JT_DOUBLE || tag == JDWP::JT_LONG) {
      expandBufAdd8BE(pReply, f->Get64(o));
    } else {
      LOG(FATAL) << "Unknown tag: " << tag;
    }
  } else {
    Object* value = f->GetObject(o);
    expandBufAdd1(pReply, TagFromObject(value));
    expandBufAddObjectId(pReply, gRegistry->Add(value));
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetFieldValue(JDWP::ObjectId objectId, JDWP::FieldId fieldId, JDWP::ExpandBuf* pReply) {
  return GetFieldValueImpl(0, objectId, fieldId, pReply, false);
}

JDWP::JdwpError Dbg::GetStaticFieldValue(JDWP::RefTypeId refTypeId, JDWP::FieldId fieldId, JDWP::ExpandBuf* pReply) {
  return GetFieldValueImpl(refTypeId, 0, fieldId, pReply, true);
}

static JDWP::JdwpError SetFieldValueImpl(JDWP::ObjectId objectId, JDWP::FieldId fieldId, uint64_t value, int width, bool is_static) {
  Object* o = gRegistry->Get<Object*>(objectId);
  if ((!is_static && o == NULL) || o == kInvalidObject) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  Field* f = FromFieldId(fieldId);

  // The RI only enforces the static/non-static mismatch in one direction.
  // TODO: should we change the tests and check both?
  if (is_static) {
    if (!f->IsStatic()) {
      return JDWP::ERR_INVALID_FIELDID;
    }
  } else {
    if (f->IsStatic()) {
      LOG(WARNING) << "Ignoring non-NULL receiver for ObjectReference.SetValues on static field " << PrettyField(f);
      o = NULL;
    }
  }

  JDWP::JdwpTag tag = BasicTagFromDescriptor(FieldHelper(f).GetTypeDescriptor());

  if (IsPrimitiveTag(tag)) {
    if (tag == JDWP::JT_DOUBLE || tag == JDWP::JT_LONG) {
      CHECK_EQ(width, 8);
      f->Set64(o, value);
    } else {
      CHECK_LE(width, 4);
      f->Set32(o, value);
    }
  } else {
    Object* v = gRegistry->Get<Object*>(value);
    if (v == kInvalidObject) {
      return JDWP::ERR_INVALID_OBJECT;
    }
    if (v != NULL) {
      Class* field_type = FieldHelper(f).GetType();
      if (!field_type->IsAssignableFrom(v->GetClass())) {
        return JDWP::ERR_INVALID_OBJECT;
      }
    }
    f->SetObject(o, v);
  }

  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::SetFieldValue(JDWP::ObjectId objectId, JDWP::FieldId fieldId, uint64_t value, int width) {
  return SetFieldValueImpl(objectId, fieldId, value, width, false);
}

JDWP::JdwpError Dbg::SetStaticFieldValue(JDWP::FieldId fieldId, uint64_t value, int width) {
  return SetFieldValueImpl(0, fieldId, value, width, true);
}

std::string Dbg::StringToUtf8(JDWP::ObjectId strId) {
  String* s = gRegistry->Get<String*>(strId);
  return s->ToModifiedUtf8();
}

bool Dbg::GetThreadName(JDWP::ObjectId threadId, std::string& name) {
  ScopedThreadListLock thread_list_lock;
  Thread* thread = DecodeThread(threadId);
  if (thread == NULL) {
    return false;
  }
  thread->GetThreadName(name);
  return true;
}

JDWP::JdwpError Dbg::GetThreadGroup(JDWP::ObjectId threadId, JDWP::ExpandBuf* pReply) {
  Object* thread = gRegistry->Get<Object*>(threadId);
  if (thread == kInvalidObject) {
    return JDWP::ERR_INVALID_OBJECT;
  }

  // Okay, so it's an object, but is it actually a thread?
  if (DecodeThread(threadId) == NULL) {
    return JDWP::ERR_INVALID_THREAD;
  }

  Class* c = Runtime::Current()->GetClassLinker()->FindSystemClass("Ljava/lang/Thread;");
  CHECK(c != NULL);
  Field* f = c->FindInstanceField("group", "Ljava/lang/ThreadGroup;");
  CHECK(f != NULL);
  Object* group = f->GetObject(thread);
  CHECK(group != NULL);
  JDWP::ObjectId thread_group_id = gRegistry->Add(group);

  expandBufAddObjectId(pReply, thread_group_id);
  return JDWP::ERR_NONE;
}

std::string Dbg::GetThreadGroupName(JDWP::ObjectId threadGroupId) {
  Object* thread_group = gRegistry->Get<Object*>(threadGroupId);
  CHECK(thread_group != NULL);

  Class* c = Runtime::Current()->GetClassLinker()->FindSystemClass("Ljava/lang/ThreadGroup;");
  CHECK(c != NULL);
  Field* f = c->FindInstanceField("name", "Ljava/lang/String;");
  CHECK(f != NULL);
  String* s = reinterpret_cast<String*>(f->GetObject(thread_group));
  return s->ToModifiedUtf8();
}

JDWP::ObjectId Dbg::GetThreadGroupParent(JDWP::ObjectId threadGroupId) {
  Object* thread_group = gRegistry->Get<Object*>(threadGroupId);
  CHECK(thread_group != NULL);

  Class* c = Runtime::Current()->GetClassLinker()->FindSystemClass("Ljava/lang/ThreadGroup;");
  CHECK(c != NULL);
  Field* f = c->FindInstanceField("parent", "Ljava/lang/ThreadGroup;");
  CHECK(f != NULL);
  Object* parent = f->GetObject(thread_group);
  return gRegistry->Add(parent);
}

JDWP::ObjectId Dbg::GetSystemThreadGroupId() {
  return gRegistry->Add(Thread::GetSystemThreadGroup());
}

JDWP::ObjectId Dbg::GetMainThreadGroupId() {
  return gRegistry->Add(Thread::GetMainThreadGroup());
}

bool Dbg::GetThreadStatus(JDWP::ObjectId threadId, JDWP::JdwpThreadStatus* pThreadStatus, JDWP::JdwpSuspendStatus* pSuspendStatus) {
  ScopedThreadListLock thread_list_lock;

  Thread* thread = DecodeThread(threadId);
  if (thread == NULL) {
    return false;
  }

  // TODO: if we're in Thread.sleep(long), we should return TS_SLEEPING,
  // even if it's implemented using Object.wait(long).
  switch (thread->GetState()) {
    case kTerminated:   *pThreadStatus = JDWP::TS_ZOMBIE;   break;
    case kRunnable:     *pThreadStatus = JDWP::TS_RUNNING;  break;
    case kTimedWaiting: *pThreadStatus = JDWP::TS_WAIT;     break;
    case kBlocked:      *pThreadStatus = JDWP::TS_MONITOR;  break;
    case kWaiting:      *pThreadStatus = JDWP::TS_WAIT;     break;
    case kStarting:     *pThreadStatus = JDWP::TS_ZOMBIE;   break;
    case kNative:       *pThreadStatus = JDWP::TS_RUNNING;  break;
    case kVmWait:       *pThreadStatus = JDWP::TS_WAIT;     break;
    case kSuspended:    *pThreadStatus = JDWP::TS_RUNNING;  break;
    // Don't add a 'default' here so the compiler can spot incompatible enum changes.
  }

  *pSuspendStatus = (thread->IsSuspended() ? JDWP::SUSPEND_STATUS_SUSPENDED : JDWP::SUSPEND_STATUS_NOT_SUSPENDED);

  return true;
}

JDWP::JdwpError Dbg::GetThreadSuspendCount(JDWP::ObjectId threadId, JDWP::ExpandBuf* pReply) {
  Thread* thread = DecodeThread(threadId);
  if (thread == NULL) {
    return JDWP::ERR_INVALID_THREAD;
  }
  expandBufAdd4BE(pReply, thread->GetSuspendCount());
  return JDWP::ERR_NONE;
}

bool Dbg::ThreadExists(JDWP::ObjectId threadId) {
  return DecodeThread(threadId) != NULL;
}

bool Dbg::IsSuspended(JDWP::ObjectId threadId) {
  return DecodeThread(threadId)->IsSuspended();
}

void Dbg::GetThreadGroupThreadsImpl(Object* thread_group, JDWP::ObjectId** ppThreadIds, uint32_t* pThreadCount) {
  struct ThreadListVisitor {
    static void Visit(Thread* t, void* arg) {
      reinterpret_cast<ThreadListVisitor*>(arg)->Visit(t);
    }

    void Visit(Thread* t) {
      if (t == Dbg::GetDebugThread()) {
        // Skip the JDWP thread. Some debuggers get bent out of shape when they can't suspend and
        // query all threads, so it's easier if we just don't tell them about this thread.
        return;
      }
      if (thread_group == NULL || t->GetThreadGroup() == thread_group) {
        threads.push_back(gRegistry->Add(t->GetPeer()));
      }
    }

    Object* thread_group;
    std::vector<JDWP::ObjectId> threads;
  };

  ThreadListVisitor tlv;
  tlv.thread_group = thread_group;

  {
    ScopedThreadListLock thread_list_lock;
    Runtime::Current()->GetThreadList()->ForEach(ThreadListVisitor::Visit, &tlv);
  }

  *pThreadCount = tlv.threads.size();
  if (*pThreadCount == 0) {
    *ppThreadIds = NULL;
  } else {
    *ppThreadIds = new JDWP::ObjectId[*pThreadCount];
    for (size_t i = 0; i < *pThreadCount; ++i) {
      (*ppThreadIds)[i] = tlv.threads[i];
    }
  }
}

void Dbg::GetThreadGroupThreads(JDWP::ObjectId threadGroupId, JDWP::ObjectId** ppThreadIds, uint32_t* pThreadCount) {
  GetThreadGroupThreadsImpl(gRegistry->Get<Object*>(threadGroupId), ppThreadIds, pThreadCount);
}

void Dbg::GetAllThreads(JDWP::ObjectId** ppThreadIds, uint32_t* pThreadCount) {
  GetThreadGroupThreadsImpl(NULL, ppThreadIds, pThreadCount);
}

static int GetStackDepth(Thread* thread) {
  struct CountStackDepthVisitor : public Thread::StackVisitor {
    CountStackDepthVisitor() : depth(0) {}
    bool VisitFrame(const Frame& f, uintptr_t) {
      if (f.HasMethod()) {
        ++depth;
      }
      return true;
    }
    size_t depth;
  };
  CountStackDepthVisitor visitor;
  thread->WalkStack(&visitor);
  return visitor.depth;
}

int Dbg::GetThreadFrameCount(JDWP::ObjectId threadId) {
  ScopedThreadListLock thread_list_lock;
  return GetStackDepth(DecodeThread(threadId));
}

void Dbg::GetThreadFrame(JDWP::ObjectId threadId, int desired_frame_number, JDWP::FrameId* pFrameId, JDWP::JdwpLocation* pLoc) {
  ScopedThreadListLock thread_list_lock;
  struct GetFrameVisitor : public Thread::StackVisitor {
    GetFrameVisitor(int desired_frame_number, JDWP::FrameId* pFrameId, JDWP::JdwpLocation* pLoc)
        : depth(0), desired_frame_number(desired_frame_number), pFrameId(pFrameId), pLoc(pLoc) {
    }
    bool VisitFrame(const Frame& f, uintptr_t pc) {
      if (!f.HasMethod()) {
        return true; // The debugger can't do anything useful with a frame that has no Method*.
      }
      if (depth == desired_frame_number) {
        *pFrameId = reinterpret_cast<JDWP::FrameId>(f.GetSP());
        SetLocation(*pLoc, f.GetMethod(), pc);
        return false;
      }
      ++depth;
      return true;
    }
    int depth;
    int desired_frame_number;
    JDWP::FrameId* pFrameId;
    JDWP::JdwpLocation* pLoc;
  };
  GetFrameVisitor visitor(desired_frame_number, pFrameId, pLoc);
  visitor.desired_frame_number = desired_frame_number;
  DecodeThread(threadId)->WalkStack(&visitor);
}

JDWP::ObjectId Dbg::GetThreadSelfId() {
  return gRegistry->Add(Thread::Current()->GetPeer());
}

void Dbg::SuspendVM() {
  ScopedThreadStateChange tsc(Thread::Current(), kRunnable); // TODO: do we really want to change back? should the JDWP thread be Runnable usually?
  Runtime::Current()->GetThreadList()->SuspendAll(true);
}

void Dbg::ResumeVM() {
  Runtime::Current()->GetThreadList()->ResumeAll(true);
}

void Dbg::SuspendThread(JDWP::ObjectId threadId) {
  Object* peer = gRegistry->Get<Object*>(threadId);
  ScopedThreadListLock thread_list_lock;
  Thread* thread = Thread::FromManagedThread(peer);
  if (thread == NULL) {
    LOG(WARNING) << "No such thread for suspend: " << peer;
    return;
  }
  Runtime::Current()->GetThreadList()->Suspend(thread, true);
}

void Dbg::ResumeThread(JDWP::ObjectId threadId) {
  Object* peer = gRegistry->Get<Object*>(threadId);
  ScopedThreadListLock thread_list_lock;
  Thread* thread = Thread::FromManagedThread(peer);
  if (thread == NULL) {
    LOG(WARNING) << "No such thread for resume: " << peer;
    return;
  }
  Runtime::Current()->GetThreadList()->Resume(thread, true);
}

void Dbg::SuspendSelf() {
  Runtime::Current()->GetThreadList()->SuspendSelfForDebugger();
}

static Object* GetThis(Frame& f) {
  Method* m = f.GetMethod();
  Object* o = NULL;
  if (!m->IsNative() && !m->IsStatic()) {
    uint16_t reg = DemangleSlot(0, m);
    o = reinterpret_cast<Object*>(f.GetVReg(m, reg));
  }
  return o;
}

void Dbg::GetThisObject(JDWP::FrameId frameId, JDWP::ObjectId* pThisId) {
  Method** sp = reinterpret_cast<Method**>(frameId);
  Frame f(sp);
  Object* o = GetThis(f);
  *pThisId = gRegistry->Add(o);
}

void Dbg::GetLocalValue(JDWP::ObjectId /*threadId*/, JDWP::FrameId frameId, int slot, JDWP::JdwpTag tag, uint8_t* buf, size_t width) {
  Method** sp = reinterpret_cast<Method**>(frameId);
  Frame f(sp);
  Method* m = f.GetMethod();
  uint16_t reg = DemangleSlot(slot, m);

#if defined(ART_USE_LLVM_COMPILER)
  UNIMPLEMENTED(FATAL);
#else
  const VmapTable vmap_table(m->GetVmapTableRaw());
  uint32_t vmap_offset;
  if (vmap_table.IsInContext(reg, vmap_offset)) {
    UNIMPLEMENTED(FATAL) << "Don't know how to pull locals from callee save frames: " << vmap_offset;
  }
#endif

  // TODO: check that the tag is compatible with the actual type of the slot!

  switch (tag) {
  case JDWP::JT_BOOLEAN:
    {
      CHECK_EQ(width, 1U);
      uint32_t intVal = f.GetVReg(m, reg);
      VLOG(jdwp) << "get boolean local " << reg << " = " << intVal;
      JDWP::Set1(buf+1, intVal != 0);
    }
    break;
  case JDWP::JT_BYTE:
    {
      CHECK_EQ(width, 1U);
      uint32_t intVal = f.GetVReg(m, reg);
      VLOG(jdwp) << "get byte local " << reg << " = " << intVal;
      JDWP::Set1(buf+1, intVal);
    }
    break;
  case JDWP::JT_SHORT:
  case JDWP::JT_CHAR:
    {
      CHECK_EQ(width, 2U);
      uint32_t intVal = f.GetVReg(m, reg);
      VLOG(jdwp) << "get short/char local " << reg << " = " << intVal;
      JDWP::Set2BE(buf+1, intVal);
    }
    break;
  case JDWP::JT_INT:
  case JDWP::JT_FLOAT:
    {
      CHECK_EQ(width, 4U);
      uint32_t intVal = f.GetVReg(m, reg);
      VLOG(jdwp) << "get int/float local " << reg << " = " << intVal;
      JDWP::Set4BE(buf+1, intVal);
    }
    break;
  case JDWP::JT_ARRAY:
    {
      CHECK_EQ(width, sizeof(JDWP::ObjectId));
      Object* o = reinterpret_cast<Object*>(f.GetVReg(m, reg));
      VLOG(jdwp) << "get array local " << reg << " = " << o;
      if (!Runtime::Current()->GetHeap()->IsHeapAddress(o)) {
        LOG(FATAL) << "Register " << reg << " expected to hold array: " << o;
      }
      JDWP::SetObjectId(buf+1, gRegistry->Add(o));
    }
    break;
  case JDWP::JT_CLASS_LOADER:
  case JDWP::JT_CLASS_OBJECT:
  case JDWP::JT_OBJECT:
  case JDWP::JT_STRING:
  case JDWP::JT_THREAD:
  case JDWP::JT_THREAD_GROUP:
    {
      CHECK_EQ(width, sizeof(JDWP::ObjectId));
      Object* o = reinterpret_cast<Object*>(f.GetVReg(m, reg));
      VLOG(jdwp) << "get object local " << reg << " = " << o;
      if (!Runtime::Current()->GetHeap()->IsHeapAddress(o)) {
        LOG(FATAL) << "Register " << reg << " expected to hold object: " << o;
      }
      tag = TagFromObject(o);
      JDWP::SetObjectId(buf+1, gRegistry->Add(o));
    }
    break;
  case JDWP::JT_DOUBLE:
  case JDWP::JT_LONG:
    {
      CHECK_EQ(width, 8U);
      uint32_t lo = f.GetVReg(m, reg);
      uint64_t hi = f.GetVReg(m, reg + 1);
      uint64_t longVal = (hi << 32) | lo;
      VLOG(jdwp) << "get double/long local " << hi << ":" << lo << " = " << longVal;
      JDWP::Set8BE(buf+1, longVal);
    }
    break;
  default:
    LOG(FATAL) << "Unknown tag " << tag;
    break;
  }

  // Prepend tag, which may have been updated.
  JDWP::Set1(buf, tag);
}

void Dbg::SetLocalValue(JDWP::ObjectId /*threadId*/, JDWP::FrameId frameId, int slot, JDWP::JdwpTag tag, uint64_t value, size_t width) {
  Method** sp = reinterpret_cast<Method**>(frameId);
  Frame f(sp);
  Method* m = f.GetMethod();
  uint16_t reg = DemangleSlot(slot, m);

#if defined(ART_USE_LLVM_COMPILER)
  UNIMPLEMENTED(FATAL);
#else
  const VmapTable vmap_table(m->GetVmapTableRaw());
  uint32_t vmap_offset;
  if (vmap_table.IsInContext(reg, vmap_offset)) {
    UNIMPLEMENTED(FATAL) << "Don't know how to pull locals from callee save frames: " << vmap_offset;
  }
#endif

  // TODO: check that the tag is compatible with the actual type of the slot!

  switch (tag) {
  case JDWP::JT_BOOLEAN:
  case JDWP::JT_BYTE:
    CHECK_EQ(width, 1U);
    f.SetVReg(m, reg, static_cast<uint32_t>(value));
    break;
  case JDWP::JT_SHORT:
  case JDWP::JT_CHAR:
    CHECK_EQ(width, 2U);
    f.SetVReg(m, reg, static_cast<uint32_t>(value));
    break;
  case JDWP::JT_INT:
  case JDWP::JT_FLOAT:
    CHECK_EQ(width, 4U);
    f.SetVReg(m, reg, static_cast<uint32_t>(value));
    break;
  case JDWP::JT_ARRAY:
  case JDWP::JT_OBJECT:
  case JDWP::JT_STRING:
    {
      CHECK_EQ(width, sizeof(JDWP::ObjectId));
      Object* o = gRegistry->Get<Object*>(static_cast<JDWP::ObjectId>(value));
      if (o == kInvalidObject) {
        UNIMPLEMENTED(FATAL) << "return an error code when given an invalid object to store";
      }
      f.SetVReg(m, reg, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(o)));
    }
    break;
  case JDWP::JT_DOUBLE:
  case JDWP::JT_LONG:
    CHECK_EQ(width, 8U);
    f.SetVReg(m, reg, static_cast<uint32_t>(value));
    f.SetVReg(m, reg + 1, static_cast<uint32_t>(value >> 32));
    break;
  default:
    LOG(FATAL) << "Unknown tag " << tag;
    break;
  }
}

void Dbg::PostLocationEvent(const Method* m, int dex_pc, Object* this_object, int event_flags) {
  Class* c = m->GetDeclaringClass();

  JDWP::JdwpLocation location;
  location.typeTag = c->IsInterface() ? JDWP::TT_INTERFACE : JDWP::TT_CLASS;
  location.classId = gRegistry->Add(c);
  location.methodId = ToMethodId(m);
  location.dex_pc = m->IsNative() ? -1 : dex_pc;

  // Note we use "NoReg" so we don't keep track of references that are
  // never actually sent to the debugger. 'this_id' is only used to
  // compare against registered events...
  JDWP::ObjectId this_id = static_cast<JDWP::ObjectId>(reinterpret_cast<uintptr_t>(this_object));
  if (gJdwpState->PostLocationEvent(&location, this_id, event_flags)) {
    // ...unless there's a registered event, in which case we
    // need to really track the class and 'this'.
    gRegistry->Add(c);
    gRegistry->Add(this_object);
  }
}

void Dbg::PostException(Method** sp, Method* throwMethod, uintptr_t throwNativePc, Method* catchMethod, uintptr_t catchNativePc, Object* exception) {
  if (!IsDebuggerActive()) {
    return;
  }

  JDWP::JdwpLocation throw_location;
  SetLocation(throw_location, throwMethod, throwNativePc);
  JDWP::JdwpLocation catch_location;
  SetLocation(catch_location, catchMethod, catchNativePc);

  // We need 'this' for InstanceOnly filters.
  JDWP::ObjectId this_id;
  GetThisObject(reinterpret_cast<JDWP::FrameId>(sp), &this_id);

  /*
   * Hand the event to the JDWP exception handler.  Note we're using the
   * "NoReg" objectID on the exception, which is not strictly correct --
   * the exception object WILL be passed up to the debugger if the
   * debugger is interested in the event.  We do this because the current
   * implementation of the debugger object registry never throws anything
   * away, and some people were experiencing a fatal build up of exception
   * objects when dealing with certain libraries.
   */
  JDWP::ObjectId exception_id = static_cast<JDWP::ObjectId>(reinterpret_cast<uintptr_t>(exception));
  JDWP::RefTypeId exception_class_id = gRegistry->Add(exception->GetClass());

  gJdwpState->PostException(&throw_location, exception_id, exception_class_id, &catch_location, this_id);
}

void Dbg::PostClassPrepare(Class* c) {
  if (!IsDebuggerActive()) {
    return;
  }

  // OLD-TODO - we currently always send both "verified" and "prepared" since
  // debuggers seem to like that.  There might be some advantage to honesty,
  // since the class may not yet be verified.
  int state = JDWP::CS_VERIFIED | JDWP::CS_PREPARED;
  JDWP::JdwpTypeTag tag = c->IsInterface() ? JDWP::TT_INTERFACE : JDWP::TT_CLASS;
  gJdwpState->PostClassPrepare(tag, gRegistry->Add(c), ClassHelper(c).GetDescriptor(), state);
}

void Dbg::UpdateDebugger(int32_t dex_pc, Thread* self, Method** sp) {
  if (!IsDebuggerActive() || dex_pc == -2 /* fake method exit */) {
    return;
  }

  Frame f(sp);
  f.Next(); // Skip callee save frame.
  Method* m = f.GetMethod();

  if (dex_pc == -1) {
    // We use a pc of -1 to represent method entry, since we might branch back to pc 0 later.
    // This means that for this special notification, there can't be anything else interesting
    // going on, so we're done already.
    Dbg::PostLocationEvent(m, 0, GetThis(f), kMethodEntry);
    return;
  }

  int event_flags = 0;

  if (IsBreakpoint(m, dex_pc)) {
    event_flags |= kBreakpoint;
  }

  // If the debugger is single-stepping one of our threads, check to
  // see if we're that thread and we've reached a step point.
  if (gSingleStepControl.is_active && gSingleStepControl.thread == self) {
    CHECK(!m->IsNative());
    if (gSingleStepControl.step_depth == JDWP::SD_INTO) {
      // Step into method calls.  We break when the line number
      // or method pointer changes.  If we're in SS_MIN mode, we
      // always stop.
      if (gSingleStepControl.method != m) {
        event_flags |= kSingleStep;
        VLOG(jdwp) << "SS new method";
      } else if (gSingleStepControl.step_size == JDWP::SS_MIN) {
        event_flags |= kSingleStep;
        VLOG(jdwp) << "SS new instruction";
      } else if (gSingleStepControl.dex_pcs.find(dex_pc) == gSingleStepControl.dex_pcs.end()) {
        event_flags |= kSingleStep;
        VLOG(jdwp) << "SS new line";
      }
    } else if (gSingleStepControl.step_depth == JDWP::SD_OVER) {
      // Step over method calls.  We break when the line number is
      // different and the frame depth is <= the original frame
      // depth.  (We can't just compare on the method, because we
      // might get unrolled past it by an exception, and it's tricky
      // to identify recursion.)

      // TODO: can we just use the value of 'sp'?
      int stack_depth = GetStackDepth(self);

      if (stack_depth < gSingleStepControl.stack_depth) {
        // popped up one or more frames, always trigger
        event_flags |= kSingleStep;
        VLOG(jdwp) << "SS method pop";
      } else if (stack_depth == gSingleStepControl.stack_depth) {
        // same depth, see if we moved
        if (gSingleStepControl.step_size == JDWP::SS_MIN) {
          event_flags |= kSingleStep;
          VLOG(jdwp) << "SS new instruction";
        } else if (gSingleStepControl.dex_pcs.find(dex_pc) == gSingleStepControl.dex_pcs.end()) {
          event_flags |= kSingleStep;
          VLOG(jdwp) << "SS new line";
        }
      }
    } else {
      CHECK_EQ(gSingleStepControl.step_depth, JDWP::SD_OUT);
      // Return from the current method.  We break when the frame
      // depth pops up.

      // This differs from the "method exit" break in that it stops
      // with the PC at the next instruction in the returned-to
      // function, rather than the end of the returning function.

      // TODO: can we just use the value of 'sp'?
      int stack_depth = GetStackDepth(self);
      if (stack_depth < gSingleStepControl.stack_depth) {
        event_flags |= kSingleStep;
        VLOG(jdwp) << "SS method pop";
      }
    }
  }

  // Check to see if this is a "return" instruction.  JDWP says we should
  // send the event *after* the code has been executed, but it also says
  // the location we provide is the last instruction.  Since the "return"
  // instruction has no interesting side effects, we should be safe.
  // (We can't just move this down to the returnFromMethod label because
  // we potentially need to combine it with other events.)
  // We're also not supposed to generate a method exit event if the method
  // terminates "with a thrown exception".
  if (dex_pc >= 0) {
    const DexFile::CodeItem* code_item = MethodHelper(m).GetCodeItem();
    CHECK(code_item != NULL);
    CHECK_LT(dex_pc, static_cast<int32_t>(code_item->insns_size_in_code_units_));
    if (Instruction::At(&code_item->insns_[dex_pc])->IsReturn()) {
      event_flags |= kMethodExit;
    }
  }

  // If there's something interesting going on, see if it matches one
  // of the debugger filters.
  if (event_flags != 0) {
    Dbg::PostLocationEvent(m, dex_pc, GetThis(f), event_flags);
  }
}

void Dbg::WatchLocation(const JDWP::JdwpLocation* location) {
  MutexLock mu(gBreakpointsLock);
  Method* m = FromMethodId(location->methodId);
  gBreakpoints.push_back(Breakpoint(m, location->dex_pc));
  VLOG(jdwp) << "Set breakpoint #" << (gBreakpoints.size() - 1) << ": " << gBreakpoints[gBreakpoints.size() - 1];
}

void Dbg::UnwatchLocation(const JDWP::JdwpLocation* location) {
  MutexLock mu(gBreakpointsLock);
  Method* m = FromMethodId(location->methodId);
  for (size_t i = 0; i < gBreakpoints.size(); ++i) {
    if (gBreakpoints[i].method == m && gBreakpoints[i].dex_pc == location->dex_pc) {
      VLOG(jdwp) << "Removed breakpoint #" << i << ": " << gBreakpoints[i];
      gBreakpoints.erase(gBreakpoints.begin() + i);
      return;
    }
  }
}

JDWP::JdwpError Dbg::ConfigureStep(JDWP::ObjectId threadId, JDWP::JdwpStepSize step_size, JDWP::JdwpStepDepth step_depth) {
  Thread* thread = DecodeThread(threadId);
  if (thread == NULL) {
    return JDWP::ERR_INVALID_THREAD;
  }

  // TODO: there's no theoretical reason why we couldn't support single-stepping
  // of multiple threads at once, but we never did so historically.
  if (gSingleStepControl.thread != NULL && thread != gSingleStepControl.thread) {
    LOG(WARNING) << "single-step already active for " << *gSingleStepControl.thread
                 << "; switching to " << *thread;
  }

  //
  // Work out what Method* we're in, the current line number, and how deep the stack currently
  // is for step-out.
  //

  struct SingleStepStackVisitor : public Thread::StackVisitor {
    SingleStepStackVisitor() {
      gSingleStepControl.method = NULL;
      gSingleStepControl.stack_depth = 0;
    }
    bool VisitFrame(const Frame& f, uintptr_t pc) {
      if (f.HasMethod()) {
        ++gSingleStepControl.stack_depth;
        if (gSingleStepControl.method == NULL) {
          const Method* m = f.GetMethod();
          const DexCache* dex_cache = m->GetDeclaringClass()->GetDexCache();
          gSingleStepControl.method = m;
          gSingleStepControl.line_number = -1;
          if (dex_cache != NULL) {
            const DexFile& dex_file = Runtime::Current()->GetClassLinker()->FindDexFile(dex_cache);
            gSingleStepControl.line_number = dex_file.GetLineNumFromPC(m, m->ToDexPC(pc));
          }
        }
      }
      return true;
    }
  };
  SingleStepStackVisitor visitor;
  thread->WalkStack(&visitor);

  //
  // Find the dex_pc values that correspond to the current line, for line-based single-stepping.
  //

  struct DebugCallbackContext {
    DebugCallbackContext() {
      last_pc_valid = false;
      last_pc = 0;
    }

    static bool Callback(void* raw_context, uint32_t address, uint32_t line_number) {
      DebugCallbackContext* context = reinterpret_cast<DebugCallbackContext*>(raw_context);
      if (static_cast<int32_t>(line_number) == gSingleStepControl.line_number) {
        if (!context->last_pc_valid) {
          // Everything from this address until the next line change is ours.
          context->last_pc = address;
          context->last_pc_valid = true;
        }
        // Otherwise, if we're already in a valid range for this line,
        // just keep going (shouldn't really happen)...
      } else if (context->last_pc_valid) { // and the line number is new
        // Add everything from the last entry up until here to the set
        for (uint32_t dex_pc = context->last_pc; dex_pc < address; ++dex_pc) {
          gSingleStepControl.dex_pcs.insert(dex_pc);
        }
        context->last_pc_valid = false;
      }
      return false; // There may be multiple entries for any given line.
    }

    ~DebugCallbackContext() {
      // If the line number was the last in the position table...
      if (last_pc_valid) {
        size_t end = MethodHelper(gSingleStepControl.method).GetCodeItem()->insns_size_in_code_units_;
        for (uint32_t dex_pc = last_pc; dex_pc < end; ++dex_pc) {
          gSingleStepControl.dex_pcs.insert(dex_pc);
        }
      }
    }

    bool last_pc_valid;
    uint32_t last_pc;
  };
  gSingleStepControl.dex_pcs.clear();
  const Method* m = gSingleStepControl.method;
  if (m->IsNative()) {
    gSingleStepControl.line_number = -1;
  } else {
    DebugCallbackContext context;
    MethodHelper mh(m);
    mh.GetDexFile().DecodeDebugInfo(mh.GetCodeItem(), m->IsStatic(), m->GetDexMethodIndex(),
                                    DebugCallbackContext::Callback, NULL, &context);
  }

  //
  // Everything else...
  //

  gSingleStepControl.thread = thread;
  gSingleStepControl.step_size = step_size;
  gSingleStepControl.step_depth = step_depth;
  gSingleStepControl.is_active = true;

  if (VLOG_IS_ON(jdwp)) {
    VLOG(jdwp) << "Single-step thread: " << *gSingleStepControl.thread;
    VLOG(jdwp) << "Single-step step size: " << gSingleStepControl.step_size;
    VLOG(jdwp) << "Single-step step depth: " << gSingleStepControl.step_depth;
    VLOG(jdwp) << "Single-step current method: " << PrettyMethod(gSingleStepControl.method);
    VLOG(jdwp) << "Single-step current line: " << gSingleStepControl.line_number;
    VLOG(jdwp) << "Single-step current stack depth: " << gSingleStepControl.stack_depth;
    VLOG(jdwp) << "Single-step dex_pc values:";
    for (std::set<uint32_t>::iterator it = gSingleStepControl.dex_pcs.begin() ; it != gSingleStepControl.dex_pcs.end(); ++it) {
      VLOG(jdwp) << StringPrintf(" %#x", *it);
    }
  }

  return JDWP::ERR_NONE;
}

void Dbg::UnconfigureStep(JDWP::ObjectId /*threadId*/) {
  gSingleStepControl.is_active = false;
  gSingleStepControl.thread = NULL;
  gSingleStepControl.dex_pcs.clear();
}

static char JdwpTagToShortyChar(JDWP::JdwpTag tag) {
  switch (tag) {
    default:
      LOG(FATAL) << "unknown JDWP tag: " << PrintableChar(tag);

    // Primitives.
    case JDWP::JT_BYTE:    return 'B';
    case JDWP::JT_CHAR:    return 'C';
    case JDWP::JT_FLOAT:   return 'F';
    case JDWP::JT_DOUBLE:  return 'D';
    case JDWP::JT_INT:     return 'I';
    case JDWP::JT_LONG:    return 'J';
    case JDWP::JT_SHORT:   return 'S';
    case JDWP::JT_VOID:    return 'V';
    case JDWP::JT_BOOLEAN: return 'Z';

    // Reference types.
    case JDWP::JT_ARRAY:
    case JDWP::JT_OBJECT:
    case JDWP::JT_STRING:
    case JDWP::JT_THREAD:
    case JDWP::JT_THREAD_GROUP:
    case JDWP::JT_CLASS_LOADER:
    case JDWP::JT_CLASS_OBJECT:
      return 'L';
  }
}

JDWP::JdwpError Dbg::InvokeMethod(JDWP::ObjectId threadId, JDWP::ObjectId objectId, JDWP::RefTypeId classId, JDWP::MethodId methodId, uint32_t arg_count, uint64_t* arg_values, JDWP::JdwpTag* arg_types, uint32_t options, JDWP::JdwpTag* pResultTag, uint64_t* pResultValue, JDWP::ObjectId* pExceptionId) {
  ThreadList* thread_list = Runtime::Current()->GetThreadList();

  Thread* targetThread = NULL;
  DebugInvokeReq* req = NULL;
  {
    ScopedThreadListLock thread_list_lock;
    targetThread = DecodeThread(threadId);
    if (targetThread == NULL) {
      LOG(ERROR) << "InvokeMethod request for non-existent thread " << threadId;
      return JDWP::ERR_INVALID_THREAD;
    }
    req = targetThread->GetInvokeReq();
    if (!req->ready) {
      LOG(ERROR) << "InvokeMethod request for thread not stopped by event: " << *targetThread;
      return JDWP::ERR_INVALID_THREAD;
    }

    /*
     * We currently have a bug where we don't successfully resume the
     * target thread if the suspend count is too deep.  We're expected to
     * require one "resume" for each "suspend", but when asked to execute
     * a method we have to resume fully and then re-suspend it back to the
     * same level.  (The easiest way to cause this is to type "suspend"
     * multiple times in jdb.)
     *
     * It's unclear what this means when the event specifies "resume all"
     * and some threads are suspended more deeply than others.  This is
     * a rare problem, so for now we just prevent it from hanging forever
     * by rejecting the method invocation request.  Without this, we will
     * be stuck waiting on a suspended thread.
     */
    int suspend_count = targetThread->GetSuspendCount();
    if (suspend_count > 1) {
      LOG(ERROR) << *targetThread << " suspend count too deep for method invocation: " << suspend_count;
      return JDWP::ERR_THREAD_SUSPENDED; // Probably not expected here.
    }

    JDWP::JdwpError status;
    Object* receiver = gRegistry->Get<Object*>(objectId);
    if (receiver == kInvalidObject) {
      return JDWP::ERR_INVALID_OBJECT;
    }

    Object* thread = gRegistry->Get<Object*>(threadId);
    if (thread == kInvalidObject) {
      return JDWP::ERR_INVALID_OBJECT;
    }
    // TODO: check that 'thread' is actually a java.lang.Thread!

    Class* c = DecodeClass(classId, status);
    if (c == NULL) {
      return status;
    }

    Method* m = FromMethodId(methodId);
    if (m->IsStatic() != (receiver == NULL)) {
      return JDWP::ERR_INVALID_METHODID;
    }
    if (m->IsStatic()) {
      if (m->GetDeclaringClass() != c) {
        return JDWP::ERR_INVALID_METHODID;
      }
    } else {
      if (!m->GetDeclaringClass()->IsAssignableFrom(c)) {
        return JDWP::ERR_INVALID_METHODID;
      }
    }

    // Check the argument list matches the method.
    MethodHelper mh(m);
    if (mh.GetShortyLength() - 1 != arg_count) {
      return JDWP::ERR_ILLEGAL_ARGUMENT;
    }
    const char* shorty = mh.GetShorty();
    for (size_t i = 0; i < arg_count; ++i) {
      if (shorty[i + 1] != JdwpTagToShortyChar(arg_types[i])) {
        return JDWP::ERR_ILLEGAL_ARGUMENT;
      }
    }

    req->receiver_ = receiver;
    req->thread_ = thread;
    req->class_ = c;
    req->method_ = m;
    req->arg_count_ = arg_count;
    req->arg_values_ = arg_values;
    req->options_ = options;
    req->invoke_needed_ = true;
  }

  // The fact that we've released the thread list lock is a bit risky --- if the thread goes
  // away we're sitting high and dry -- but we must release this before the ResumeAllThreads
  // call, and it's unwise to hold it during WaitForSuspend.

  {
    /*
     * We change our (JDWP thread) status, which should be THREAD_RUNNING,
     * so we can suspend for a GC if the invoke request causes us to
     * run out of memory.  It's also a good idea to change it before locking
     * the invokeReq mutex, although that should never be held for long.
     */
    ScopedThreadStateChange tsc(Thread::Current(), kVmWait);

    VLOG(jdwp) << "    Transferring control to event thread";
    {
      MutexLock mu(req->lock_);

      if ((options & JDWP::INVOKE_SINGLE_THREADED) == 0) {
        VLOG(jdwp) << "      Resuming all threads";
        thread_list->ResumeAll(true);
      } else {
        VLOG(jdwp) << "      Resuming event thread only";
        thread_list->Resume(targetThread, true);
      }

      // Wait for the request to finish executing.
      while (req->invoke_needed_) {
        req->cond_.Wait(req->lock_);
      }
    }
    VLOG(jdwp) << "    Control has returned from event thread";

    /* wait for thread to re-suspend itself */
    targetThread->WaitUntilSuspended();
    //dvmWaitForSuspend(targetThread);
  }

  /*
   * Suspend the threads.  We waited for the target thread to suspend
   * itself, so all we need to do is suspend the others.
   *
   * The suspendAllThreads() call will double-suspend the event thread,
   * so we want to resume the target thread once to keep the books straight.
   */
  if ((options & JDWP::INVOKE_SINGLE_THREADED) == 0) {
    VLOG(jdwp) << "      Suspending all threads";
    thread_list->SuspendAll(true);
    VLOG(jdwp) << "      Resuming event thread to balance the count";
    thread_list->Resume(targetThread, true);
  }

  // Copy the result.
  *pResultTag = req->result_tag;
  if (IsPrimitiveTag(req->result_tag)) {
    *pResultValue = req->result_value.GetJ();
  } else {
    *pResultValue = gRegistry->Add(req->result_value.GetL());
  }
  *pExceptionId = req->exception;
  return req->error;
}

void Dbg::ExecuteMethod(DebugInvokeReq* pReq) {
  Thread* self = Thread::Current();

  // We can be called while an exception is pending. We need
  // to preserve that across the method invocation.
  SirtRef<Throwable> old_exception(self->GetException());
  self->ClearException();

  ScopedThreadStateChange tsc(self, kRunnable);

  // Translate the method through the vtable, unless the debugger wants to suppress it.
  Method* m = pReq->method_;
  if ((pReq->options_ & JDWP::INVOKE_NONVIRTUAL) == 0 && pReq->receiver_ != NULL) {
    Method* actual_method = pReq->class_->FindVirtualMethodForVirtualOrInterface(pReq->method_);
    if (actual_method != m) {
      VLOG(jdwp) << "ExecuteMethod translated " << PrettyMethod(m) << " to " << PrettyMethod(actual_method);
      m = actual_method;
    }
  }
  VLOG(jdwp) << "ExecuteMethod " << PrettyMethod(m);
  CHECK(m != NULL);

  CHECK_EQ(sizeof(jvalue), sizeof(uint64_t));

  LOG(INFO) << "self=" << self << " pReq->receiver_=" << pReq->receiver_ << " m=" << m << " #" << pReq->arg_count_ << " " << pReq->arg_values_;
  pReq->result_value = InvokeWithJValues(self, pReq->receiver_, m, reinterpret_cast<JValue*>(pReq->arg_values_));

  pReq->exception = gRegistry->Add(self->GetException());
  pReq->result_tag = BasicTagFromDescriptor(MethodHelper(m).GetShorty());
  if (pReq->exception != 0) {
    Object* exc = self->GetException();
    VLOG(jdwp) << "  JDWP invocation returning with exception=" << exc << " " << PrettyTypeOf(exc);
    self->ClearException();
    pReq->result_value.SetJ(0);
  } else if (pReq->result_tag == JDWP::JT_OBJECT) {
    /* if no exception thrown, examine object result more closely */
    JDWP::JdwpTag new_tag = TagFromObject(pReq->result_value.GetL());
    if (new_tag != pReq->result_tag) {
      VLOG(jdwp) << "  JDWP promoted result from " << pReq->result_tag << " to " << new_tag;
      pReq->result_tag = new_tag;
    }

    /*
     * Register the object.  We don't actually need an ObjectId yet,
     * but we do need to be sure that the GC won't move or discard the
     * object when we switch out of RUNNING.  The ObjectId conversion
     * will add the object to the "do not touch" list.
     *
     * We can't use the "tracked allocation" mechanism here because
     * the object is going to be handed off to a different thread.
     */
    gRegistry->Add(pReq->result_value.GetL());
  }

  if (old_exception.get() != NULL) {
    self->SetException(old_exception.get());
  }
}

/*
 * Register an object ID that might not have been registered previously.
 *
 * Normally this wouldn't happen -- the conversion to an ObjectId would
 * have added the object to the registry -- but in some cases (e.g.
 * throwing exceptions) we really want to do the registration late.
 */
void Dbg::RegisterObjectId(JDWP::ObjectId id) {
  gRegistry->Add(reinterpret_cast<Object*>(id));
}

/*
 * "buf" contains a full JDWP packet, possibly with multiple chunks.  We
 * need to process each, accumulate the replies, and ship the whole thing
 * back.
 *
 * Returns "true" if we have a reply.  The reply buffer is newly allocated,
 * and includes the chunk type/length, followed by the data.
 *
 * OLD-TODO: we currently assume that the request and reply include a single
 * chunk.  If this becomes inconvenient we will need to adapt.
 */
bool Dbg::DdmHandlePacket(const uint8_t* buf, int dataLen, uint8_t** pReplyBuf, int* pReplyLen) {
  CHECK_GE(dataLen, 0);

  Thread* self = Thread::Current();
  JNIEnv* env = self->GetJniEnv();

  // Create a byte[] corresponding to 'buf'.
  ScopedLocalRef<jbyteArray> dataArray(env, env->NewByteArray(dataLen));
  if (dataArray.get() == NULL) {
    LOG(WARNING) << "byte[] allocation failed: " << dataLen;
    env->ExceptionClear();
    return false;
  }
  env->SetByteArrayRegion(dataArray.get(), 0, dataLen, reinterpret_cast<const jbyte*>(buf));

  const int kChunkHdrLen = 8;

  // Run through and find all chunks.  [Currently just find the first.]
  ScopedByteArrayRO contents(env, dataArray.get());
  jint type = JDWP::Get4BE(reinterpret_cast<const uint8_t*>(&contents[0]));
  jint length = JDWP::Get4BE(reinterpret_cast<const uint8_t*>(&contents[4]));
  jint offset = kChunkHdrLen;
  if (offset + length > dataLen) {
    LOG(WARNING) << StringPrintf("bad chunk found (len=%u pktLen=%d)", length, dataLen);
    return false;
  }

  // Call "private static Chunk dispatch(int type, byte[] data, int offset, int length)".
  ScopedLocalRef<jobject> chunk(env, env->CallStaticObjectMethod(WellKnownClasses::org_apache_harmony_dalvik_ddmc_DdmServer,
                                                                 WellKnownClasses::org_apache_harmony_dalvik_ddmc_DdmServer_dispatch,
                                                                 type, dataArray.get(), offset, length));
  if (env->ExceptionCheck()) {
    LOG(INFO) << StringPrintf("Exception thrown by dispatcher for 0x%08x", type);
    env->ExceptionDescribe();
    env->ExceptionClear();
    return false;
  }

  if (chunk.get() == NULL) {
    return false;
  }

  /*
   * Pull the pieces out of the chunk.  We copy the results into a
   * newly-allocated buffer that the caller can free.  We don't want to
   * continue using the Chunk object because nothing has a reference to it.
   *
   * We could avoid this by returning type/data/offset/length and having
   * the caller be aware of the object lifetime issues, but that
   * integrates the JDWP code more tightly into the rest of the runtime, and doesn't work
   * if we have responses for multiple chunks.
   *
   * So we're pretty much stuck with copying data around multiple times.
   */
  ScopedLocalRef<jbyteArray> replyData(env, reinterpret_cast<jbyteArray>(env->GetObjectField(chunk.get(), WellKnownClasses::org_apache_harmony_dalvik_ddmc_Chunk_data)));
  length = env->GetIntField(chunk.get(), WellKnownClasses::org_apache_harmony_dalvik_ddmc_Chunk_length);
  offset = env->GetIntField(chunk.get(), WellKnownClasses::org_apache_harmony_dalvik_ddmc_Chunk_offset);
  type = env->GetIntField(chunk.get(), WellKnownClasses::org_apache_harmony_dalvik_ddmc_Chunk_type);

  VLOG(jdwp) << StringPrintf("DDM reply: type=0x%08x data=%p offset=%d length=%d", type, replyData.get(), offset, length);
  if (length == 0 || replyData.get() == NULL) {
    return false;
  }

  jsize replyLength = env->GetArrayLength(replyData.get());
  if (offset + length > replyLength) {
    LOG(WARNING) << StringPrintf("chunk off=%d len=%d exceeds reply array len %d", offset, length, replyLength);
    return false;
  }

  uint8_t* reply = new uint8_t[length + kChunkHdrLen];
  if (reply == NULL) {
    LOG(WARNING) << "malloc failed: " << (length + kChunkHdrLen);
    return false;
  }
  JDWP::Set4BE(reply + 0, type);
  JDWP::Set4BE(reply + 4, length);
  env->GetByteArrayRegion(replyData.get(), offset, length, reinterpret_cast<jbyte*>(reply + kChunkHdrLen));

  *pReplyBuf = reply;
  *pReplyLen = length + kChunkHdrLen;

  VLOG(jdwp) << StringPrintf("dvmHandleDdm returning type=%.4s buf=%p len=%d", reinterpret_cast<char*>(reply), reply, length);
  return true;
}

void Dbg::DdmBroadcast(bool connect) {
  VLOG(jdwp) << "Broadcasting DDM " << (connect ? "connect" : "disconnect") << "...";

  Thread* self = Thread::Current();
  if (self->GetState() != kRunnable) {
    LOG(ERROR) << "DDM broadcast in thread state " << self->GetState();
    /* try anyway? */
  }

  JNIEnv* env = self->GetJniEnv();
  jint event = connect ? 1 /*DdmServer.CONNECTED*/ : 2 /*DdmServer.DISCONNECTED*/;
  env->CallStaticVoidMethod(WellKnownClasses::org_apache_harmony_dalvik_ddmc_DdmServer,
                            WellKnownClasses::org_apache_harmony_dalvik_ddmc_DdmServer_broadcast,
                            event);
  if (env->ExceptionCheck()) {
    LOG(ERROR) << "DdmServer.broadcast " << event << " failed";
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
}

void Dbg::DdmConnected() {
  Dbg::DdmBroadcast(true);
}

void Dbg::DdmDisconnected() {
  Dbg::DdmBroadcast(false);
  gDdmThreadNotification = false;
}

/*
 * Send a notification when a thread starts, stops, or changes its name.
 *
 * Because we broadcast the full set of threads when the notifications are
 * first enabled, it's possible for "thread" to be actively executing.
 */
void Dbg::DdmSendThreadNotification(Thread* t, uint32_t type) {
  if (!gDdmThreadNotification) {
    return;
  }

  if (type == CHUNK_TYPE("THDE")) {
    uint8_t buf[4];
    JDWP::Set4BE(&buf[0], t->GetThinLockId());
    Dbg::DdmSendChunk(CHUNK_TYPE("THDE"), 4, buf);
  } else {
    CHECK(type == CHUNK_TYPE("THCR") || type == CHUNK_TYPE("THNM")) << type;
    SirtRef<String> name(t->GetThreadName());
    size_t char_count = (name.get() != NULL) ? name->GetLength() : 0;
    const jchar* chars = name->GetCharArray()->GetData();

    std::vector<uint8_t> bytes;
    JDWP::Append4BE(bytes, t->GetThinLockId());
    JDWP::AppendUtf16BE(bytes, chars, char_count);
    CHECK_EQ(bytes.size(), char_count*2 + sizeof(uint32_t)*2);
    Dbg::DdmSendChunk(type, bytes);
  }
}

static void DdmSendThreadStartCallback(Thread* t, void*) {
  Dbg::DdmSendThreadNotification(t, CHUNK_TYPE("THCR"));
}

void Dbg::DdmSetThreadNotification(bool enable) {
  // We lock the thread list to avoid sending duplicate events or missing
  // a thread change. We should be okay holding this lock while sending
  // the messages out. (We have to hold it while accessing a live thread.)
  ScopedThreadListLock thread_list_lock;

  gDdmThreadNotification = enable;
  if (enable) {
    Runtime::Current()->GetThreadList()->ForEach(DdmSendThreadStartCallback, NULL);
  }
}

void Dbg::PostThreadStartOrStop(Thread* t, uint32_t type) {
  if (IsDebuggerActive()) {
    JDWP::ObjectId id = gRegistry->Add(t->GetPeer());
    gJdwpState->PostThreadChange(id, type == CHUNK_TYPE("THCR"));
    // If this thread's just joined the party while we're already debugging, make sure it knows
    // to give us updates when it's running.
    t->SetDebuggerUpdatesEnabled(true);
  }
  Dbg::DdmSendThreadNotification(t, type);
}

void Dbg::PostThreadStart(Thread* t) {
  Dbg::PostThreadStartOrStop(t, CHUNK_TYPE("THCR"));
}

void Dbg::PostThreadDeath(Thread* t) {
  Dbg::PostThreadStartOrStop(t, CHUNK_TYPE("THDE"));
}

void Dbg::DdmSendChunk(uint32_t type, size_t byte_count, const uint8_t* buf) {
  CHECK(buf != NULL);
  iovec vec[1];
  vec[0].iov_base = reinterpret_cast<void*>(const_cast<uint8_t*>(buf));
  vec[0].iov_len = byte_count;
  Dbg::DdmSendChunkV(type, vec, 1);
}

void Dbg::DdmSendChunk(uint32_t type, const std::vector<uint8_t>& bytes) {
  DdmSendChunk(type, bytes.size(), &bytes[0]);
}

void Dbg::DdmSendChunkV(uint32_t type, const struct iovec* iov, int iov_count) {
  if (gJdwpState == NULL) {
    VLOG(jdwp) << "Debugger thread not active, ignoring DDM send: " << type;
  } else {
    gJdwpState->DdmSendChunkV(type, iov, iov_count);
  }
}

int Dbg::DdmHandleHpifChunk(HpifWhen when) {
  if (when == HPIF_WHEN_NOW) {
    DdmSendHeapInfo(when);
    return true;
  }

  if (when != HPIF_WHEN_NEVER && when != HPIF_WHEN_NEXT_GC && when != HPIF_WHEN_EVERY_GC) {
    LOG(ERROR) << "invalid HpifWhen value: " << static_cast<int>(when);
    return false;
  }

  gDdmHpifWhen = when;
  return true;
}

bool Dbg::DdmHandleHpsgNhsgChunk(Dbg::HpsgWhen when, Dbg::HpsgWhat what, bool native) {
  if (when != HPSG_WHEN_NEVER && when != HPSG_WHEN_EVERY_GC) {
    LOG(ERROR) << "invalid HpsgWhen value: " << static_cast<int>(when);
    return false;
  }

  if (what != HPSG_WHAT_MERGED_OBJECTS && what != HPSG_WHAT_DISTINCT_OBJECTS) {
    LOG(ERROR) << "invalid HpsgWhat value: " << static_cast<int>(what);
    return false;
  }

  if (native) {
    gDdmNhsgWhen = when;
    gDdmNhsgWhat = what;
  } else {
    gDdmHpsgWhen = when;
    gDdmHpsgWhat = what;
  }
  return true;
}

void Dbg::DdmSendHeapInfo(HpifWhen reason) {
  // If there's a one-shot 'when', reset it.
  if (reason == gDdmHpifWhen) {
    if (gDdmHpifWhen == HPIF_WHEN_NEXT_GC) {
      gDdmHpifWhen = HPIF_WHEN_NEVER;
    }
  }

  /*
   * Chunk HPIF (client --> server)
   *
   * Heap Info. General information about the heap,
   * suitable for a summary display.
   *
   *   [u4]: number of heaps
   *
   *   For each heap:
   *     [u4]: heap ID
   *     [u8]: timestamp in ms since Unix epoch
   *     [u1]: capture reason (same as 'when' value from server)
   *     [u4]: max heap size in bytes (-Xmx)
   *     [u4]: current heap size in bytes
   *     [u4]: current number of bytes allocated
   *     [u4]: current number of objects allocated
   */
  uint8_t heap_count = 1;
  Heap* heap = Runtime::Current()->GetHeap();
  std::vector<uint8_t> bytes;
  JDWP::Append4BE(bytes, heap_count);
  JDWP::Append4BE(bytes, 1); // Heap id (bogus; we only have one heap).
  JDWP::Append8BE(bytes, MilliTime());
  JDWP::Append1BE(bytes, reason);
  JDWP::Append4BE(bytes, heap->GetMaxMemory()); // Max allowed heap size in bytes.
  JDWP::Append4BE(bytes, heap->GetTotalMemory()); // Current heap size in bytes.
  JDWP::Append4BE(bytes, heap->GetBytesAllocated());
  JDWP::Append4BE(bytes, heap->GetObjectsAllocated());
  CHECK_EQ(bytes.size(), 4U + (heap_count * (4 + 8 + 1 + 4 + 4 + 4 + 4)));
  Dbg::DdmSendChunk(CHUNK_TYPE("HPIF"), bytes);
}

enum HpsgSolidity {
  SOLIDITY_FREE = 0,
  SOLIDITY_HARD = 1,
  SOLIDITY_SOFT = 2,
  SOLIDITY_WEAK = 3,
  SOLIDITY_PHANTOM = 4,
  SOLIDITY_FINALIZABLE = 5,
  SOLIDITY_SWEEP = 6,
};

enum HpsgKind {
  KIND_OBJECT = 0,
  KIND_CLASS_OBJECT = 1,
  KIND_ARRAY_1 = 2,
  KIND_ARRAY_2 = 3,
  KIND_ARRAY_4 = 4,
  KIND_ARRAY_8 = 5,
  KIND_UNKNOWN = 6,
  KIND_NATIVE = 7,
};

#define HPSG_PARTIAL (1<<7)
#define HPSG_STATE(solidity, kind) ((uint8_t)((((kind) & 0x7) << 3) | ((solidity) & 0x7)))

class HeapChunkContext {
 public:
  // Maximum chunk size.  Obtain this from the formula:
  // (((maximum_heap_size / ALLOCATION_UNIT_SIZE) + 255) / 256) * 2
  HeapChunkContext(bool merge, bool native)
      : buf_(16384 - 16),
        type_(0),
        merge_(merge) {
    Reset();
    if (native) {
      type_ = CHUNK_TYPE("NHSG");
    } else {
      type_ = merge ? CHUNK_TYPE("HPSG") : CHUNK_TYPE("HPSO");
    }
  }

  ~HeapChunkContext() {
    if (p_ > &buf_[0]) {
      Flush();
    }
  }

  void EnsureHeader(const void* chunk_ptr) {
    if (!needHeader_) {
      return;
    }

    // Start a new HPSx chunk.
    JDWP::Write4BE(&p_, 1); // Heap id (bogus; we only have one heap).
    JDWP::Write1BE(&p_, 8); // Size of allocation unit, in bytes.

    JDWP::Write4BE(&p_, reinterpret_cast<uintptr_t>(chunk_ptr)); // virtual address of segment start.
    JDWP::Write4BE(&p_, 0); // offset of this piece (relative to the virtual address).
    // [u4]: length of piece, in allocation units
    // We won't know this until we're done, so save the offset and stuff in a dummy value.
    pieceLenField_ = p_;
    JDWP::Write4BE(&p_, 0x55555555);
    needHeader_ = false;
  }

  void Flush() {
    // Patch the "length of piece" field.
    CHECK_LE(&buf_[0], pieceLenField_);
    CHECK_LE(pieceLenField_, p_);
    JDWP::Set4BE(pieceLenField_, totalAllocationUnits_);

    Dbg::DdmSendChunk(type_, p_ - &buf_[0], &buf_[0]);
    Reset();
  }

  static void HeapChunkCallback(void* start, void* end, size_t used_bytes, void* arg) {
    reinterpret_cast<HeapChunkContext*>(arg)->HeapChunkCallback(start, end, used_bytes);
  }

 private:
  enum { ALLOCATION_UNIT_SIZE = 8 };

  void Reset() {
    p_ = &buf_[0];
    totalAllocationUnits_ = 0;
    needHeader_ = true;
    pieceLenField_ = NULL;
  }

  void HeapChunkCallback(void* start, void* /*end*/, size_t used_bytes) {
    // Note: heap call backs cannot manipulate the heap upon which they are crawling, care is taken
    // in the following code not to allocate memory, by ensuring buf_ is of the correct size

    void* user_ptr = used_bytes > 0 ? start : NULL;
    size_t chunk_len = mspace_usable_size(user_ptr);

    // Make sure there's enough room left in the buffer.
    // We need to use two bytes for every fractional 256 allocation units used by the chunk.
    {
      size_t needed = (((chunk_len/ALLOCATION_UNIT_SIZE + 255) / 256) * 2);
      size_t bytesLeft = buf_.size() - (size_t)(p_ - &buf_[0]);
      if (bytesLeft < needed) {
        Flush();
      }

      bytesLeft = buf_.size() - (size_t)(p_ - &buf_[0]);
      if (bytesLeft < needed) {
        LOG(WARNING) << "Chunk is too big to transmit (chunk_len=" << chunk_len << ", " << needed << " bytes)";
        return;
      }
    }

    // OLD-TODO: notice when there's a gap and start a new heap, or at least a new range.
    EnsureHeader(start);

    // Determine the type of this chunk.
    // OLD-TODO: if context.merge, see if this chunk is different from the last chunk.
    // If it's the same, we should combine them.
    uint8_t state = ExamineObject(reinterpret_cast<const Object*>(user_ptr), (type_ == CHUNK_TYPE("NHSG")));

    // Write out the chunk description.
    chunk_len /= ALLOCATION_UNIT_SIZE;   // convert to allocation units
    totalAllocationUnits_ += chunk_len;
    while (chunk_len > 256) {
      *p_++ = state | HPSG_PARTIAL;
      *p_++ = 255;     // length - 1
      chunk_len -= 256;
    }
    *p_++ = state;
    *p_++ = chunk_len - 1;
  }

  uint8_t ExamineObject(const Object* o, bool is_native_heap) {
    if (o == NULL) {
      return HPSG_STATE(SOLIDITY_FREE, 0);
    }

    // It's an allocated chunk. Figure out what it is.

    // If we're looking at the native heap, we'll just return
    // (SOLIDITY_HARD, KIND_NATIVE) for all allocated chunks.
    if (is_native_heap || !Runtime::Current()->GetHeap()->IsLiveObjectLocked(o)) {
      return HPSG_STATE(SOLIDITY_HARD, KIND_NATIVE);
    }

    Class* c = o->GetClass();
    if (c == NULL) {
      // The object was probably just created but hasn't been initialized yet.
      return HPSG_STATE(SOLIDITY_HARD, KIND_OBJECT);
    }

    if (!Runtime::Current()->GetHeap()->IsHeapAddress(c)) {
      LOG(WARNING) << "Invalid class for managed heap object: " << o << " " << c;
      return HPSG_STATE(SOLIDITY_HARD, KIND_UNKNOWN);
    }

    if (c->IsClassClass()) {
      return HPSG_STATE(SOLIDITY_HARD, KIND_CLASS_OBJECT);
    }

    if (c->IsArrayClass()) {
      if (o->IsObjectArray()) {
        return HPSG_STATE(SOLIDITY_HARD, KIND_ARRAY_4);
      }
      switch (c->GetComponentSize()) {
      case 1: return HPSG_STATE(SOLIDITY_HARD, KIND_ARRAY_1);
      case 2: return HPSG_STATE(SOLIDITY_HARD, KIND_ARRAY_2);
      case 4: return HPSG_STATE(SOLIDITY_HARD, KIND_ARRAY_4);
      case 8: return HPSG_STATE(SOLIDITY_HARD, KIND_ARRAY_8);
      }
    }

    return HPSG_STATE(SOLIDITY_HARD, KIND_OBJECT);
  }

  std::vector<uint8_t> buf_;
  uint8_t* p_;
  uint8_t* pieceLenField_;
  size_t totalAllocationUnits_;
  uint32_t type_;
  bool merge_;
  bool needHeader_;

  DISALLOW_COPY_AND_ASSIGN(HeapChunkContext);
};

void Dbg::DdmSendHeapSegments(bool native) {
  Dbg::HpsgWhen when;
  Dbg::HpsgWhat what;
  if (!native) {
    when = gDdmHpsgWhen;
    what = gDdmHpsgWhat;
  } else {
    when = gDdmNhsgWhen;
    what = gDdmNhsgWhat;
  }
  if (when == HPSG_WHEN_NEVER) {
    return;
  }

  // Figure out what kind of chunks we'll be sending.
  CHECK(what == HPSG_WHAT_MERGED_OBJECTS || what == HPSG_WHAT_DISTINCT_OBJECTS) << static_cast<int>(what);

  // First, send a heap start chunk.
  uint8_t heap_id[4];
  JDWP::Set4BE(&heap_id[0], 1); // Heap id (bogus; we only have one heap).
  Dbg::DdmSendChunk(native ? CHUNK_TYPE("NHST") : CHUNK_TYPE("HPST"), sizeof(heap_id), heap_id);

  // Send a series of heap segment chunks.
  HeapChunkContext context((what == HPSG_WHAT_MERGED_OBJECTS), native);
  if (native) {
    // TODO: enable when bionic has moved to dlmalloc 2.8.5
    // dlmalloc_inspect_all(HeapChunkContext::HeapChunkCallback, &context);
    UNIMPLEMENTED(WARNING) << "Native heap send heap segments";
  } else {
    Heap* heap = Runtime::Current()->GetHeap();
    heap->GetAllocSpace()->Walk(HeapChunkContext::HeapChunkCallback, &context);
  }

  // Finally, send a heap end chunk.
  Dbg::DdmSendChunk(native ? CHUNK_TYPE("NHEN") : CHUNK_TYPE("HPEN"), sizeof(heap_id), heap_id);
}

void Dbg::SetAllocTrackingEnabled(bool enabled) {
  MutexLock mu(gAllocTrackerLock);
  if (enabled) {
    if (recent_allocation_records_ == NULL) {
      LOG(INFO) << "Enabling alloc tracker (" << kNumAllocRecords << " entries, "
                << kMaxAllocRecordStackDepth << " frames --> "
                << (sizeof(AllocRecord) * kNumAllocRecords) << " bytes)";
      gAllocRecordHead = gAllocRecordCount = 0;
      recent_allocation_records_ = new AllocRecord[kNumAllocRecords];
      CHECK(recent_allocation_records_ != NULL);
    }
  } else {
    delete[] recent_allocation_records_;
    recent_allocation_records_ = NULL;
  }
}

struct AllocRecordStackVisitor : public Thread::StackVisitor {
  explicit AllocRecordStackVisitor(AllocRecord* record) : record(record), depth(0) {
  }

  bool VisitFrame(const Frame& f, uintptr_t pc) {
    if (depth >= kMaxAllocRecordStackDepth) {
      return false;
    }
    if (f.HasMethod()) {
      record->stack[depth].method = f.GetMethod();
      record->stack[depth].raw_pc = pc;
      ++depth;
    }
    return true;
  }

  ~AllocRecordStackVisitor() {
    // Clear out any unused stack trace elements.
    for (; depth < kMaxAllocRecordStackDepth; ++depth) {
      record->stack[depth].method = NULL;
      record->stack[depth].raw_pc = 0;
    }
  }

  AllocRecord* record;
  size_t depth;
};

void Dbg::RecordAllocation(Class* type, size_t byte_count) {
  Thread* self = Thread::Current();
  CHECK(self != NULL);

  MutexLock mu(gAllocTrackerLock);
  if (recent_allocation_records_ == NULL) {
    return;
  }

  // Advance and clip.
  if (++gAllocRecordHead == kNumAllocRecords) {
    gAllocRecordHead = 0;
  }

  // Fill in the basics.
  AllocRecord* record = &recent_allocation_records_[gAllocRecordHead];
  record->type = type;
  record->byte_count = byte_count;
  record->thin_lock_id = self->GetThinLockId();

  // Fill in the stack trace.
  AllocRecordStackVisitor visitor(record);
  self->WalkStack(&visitor);

  if (gAllocRecordCount < kNumAllocRecords) {
    ++gAllocRecordCount;
  }
}

/*
 * Return the index of the head element.
 *
 * We point at the most-recently-written record, so if allocRecordCount is 1
 * we want to use the current element.  Take "head+1" and subtract count
 * from it.
 *
 * We need to handle underflow in our circular buffer, so we add
 * kNumAllocRecords and then mask it back down.
 */
inline static int headIndex() {
  return (gAllocRecordHead+1 + kNumAllocRecords - gAllocRecordCount) & (kNumAllocRecords-1);
}

void Dbg::DumpRecentAllocations() {
  MutexLock mu(gAllocTrackerLock);
  if (recent_allocation_records_ == NULL) {
    LOG(INFO) << "Not recording tracked allocations";
    return;
  }

  // "i" is the head of the list.  We want to start at the end of the
  // list and move forward to the tail.
  size_t i = headIndex();
  size_t count = gAllocRecordCount;

  LOG(INFO) << "Tracked allocations, (head=" << gAllocRecordHead << " count=" << count << ")";
  while (count--) {
    AllocRecord* record = &recent_allocation_records_[i];

    LOG(INFO) << StringPrintf(" T=%-2d %6zd ", record->thin_lock_id, record->byte_count)
              << PrettyClass(record->type);

    for (size_t stack_frame = 0; stack_frame < kMaxAllocRecordStackDepth; ++stack_frame) {
      const Method* m = record->stack[stack_frame].method;
      if (m == NULL) {
        break;
      }
      LOG(INFO) << "    " << PrettyMethod(m) << " line " << record->stack[stack_frame].LineNumber();
    }

    // pause periodically to help logcat catch up
    if ((count % 5) == 0) {
      usleep(40000);
    }

    i = (i + 1) & (kNumAllocRecords-1);
  }
}

class StringTable {
 public:
  StringTable() {
  }

  void Add(const char* s) {
    table_.insert(s);
  }

  size_t IndexOf(const char* s) {
    return std::distance(table_.begin(), table_.find(s));
  }

  size_t Size() {
    return table_.size();
  }

  void WriteTo(std::vector<uint8_t>& bytes) {
    typedef std::set<const char*>::const_iterator It; // TODO: C++0x auto
    for (It it = table_.begin(); it != table_.end(); ++it) {
      const char* s = *it;
      size_t s_len = CountModifiedUtf8Chars(s);
      UniquePtr<uint16_t> s_utf16(new uint16_t[s_len]);
      ConvertModifiedUtf8ToUtf16(s_utf16.get(), s);
      JDWP::AppendUtf16BE(bytes, s_utf16.get(), s_len);
    }
  }

 private:
  std::set<const char*> table_;
  DISALLOW_COPY_AND_ASSIGN(StringTable);
};

/*
 * The data we send to DDMS contains everything we have recorded.
 *
 * Message header (all values big-endian):
 * (1b) message header len (to allow future expansion); includes itself
 * (1b) entry header len
 * (1b) stack frame len
 * (2b) number of entries
 * (4b) offset to string table from start of message
 * (2b) number of class name strings
 * (2b) number of method name strings
 * (2b) number of source file name strings
 * For each entry:
 *   (4b) total allocation size
 *   (2b) threadId
 *   (2b) allocated object's class name index
 *   (1b) stack depth
 *   For each stack frame:
 *     (2b) method's class name
 *     (2b) method name
 *     (2b) method source file
 *     (2b) line number, clipped to 32767; -2 if native; -1 if no source
 * (xb) class name strings
 * (xb) method name strings
 * (xb) source file strings
 *
 * As with other DDM traffic, strings are sent as a 4-byte length
 * followed by UTF-16 data.
 *
 * We send up 16-bit unsigned indexes into string tables.  In theory there
 * can be (kMaxAllocRecordStackDepth * kNumAllocRecords) unique strings in
 * each table, but in practice there should be far fewer.
 *
 * The chief reason for using a string table here is to keep the size of
 * the DDMS message to a minimum.  This is partly to make the protocol
 * efficient, but also because we have to form the whole thing up all at
 * once in a memory buffer.
 *
 * We use separate string tables for class names, method names, and source
 * files to keep the indexes small.  There will generally be no overlap
 * between the contents of these tables.
 */
jbyteArray Dbg::GetRecentAllocations() {
  if (false) {
    DumpRecentAllocations();
  }

  MutexLock mu(gAllocTrackerLock);

  /*
   * Part 1: generate string tables.
   */
  StringTable class_names;
  StringTable method_names;
  StringTable filenames;

  int count = gAllocRecordCount;
  int idx = headIndex();
  while (count--) {
    AllocRecord* record = &recent_allocation_records_[idx];

    class_names.Add(ClassHelper(record->type).GetDescriptor());

    MethodHelper mh;
    for (size_t i = 0; i < kMaxAllocRecordStackDepth; i++) {
      Method* m = record->stack[i].method;
      if (m != NULL) {
        mh.ChangeMethod(m);
        class_names.Add(mh.GetDeclaringClassDescriptor());
        method_names.Add(mh.GetName());
        filenames.Add(mh.GetDeclaringClassSourceFile());
      }
    }

    idx = (idx + 1) & (kNumAllocRecords-1);
  }

  LOG(INFO) << "allocation records: " << gAllocRecordCount;

  /*
   * Part 2: allocate a buffer and generate the output.
   */
  std::vector<uint8_t> bytes;

  // (1b) message header len (to allow future expansion); includes itself
  // (1b) entry header len
  // (1b) stack frame len
  const int kMessageHeaderLen = 15;
  const int kEntryHeaderLen = 9;
  const int kStackFrameLen = 8;
  JDWP::Append1BE(bytes, kMessageHeaderLen);
  JDWP::Append1BE(bytes, kEntryHeaderLen);
  JDWP::Append1BE(bytes, kStackFrameLen);

  // (2b) number of entries
  // (4b) offset to string table from start of message
  // (2b) number of class name strings
  // (2b) number of method name strings
  // (2b) number of source file name strings
  JDWP::Append2BE(bytes, gAllocRecordCount);
  size_t string_table_offset = bytes.size();
  JDWP::Append4BE(bytes, 0); // We'll patch this later...
  JDWP::Append2BE(bytes, class_names.Size());
  JDWP::Append2BE(bytes, method_names.Size());
  JDWP::Append2BE(bytes, filenames.Size());

  count = gAllocRecordCount;
  idx = headIndex();
  ClassHelper kh;
  while (count--) {
    // For each entry:
    // (4b) total allocation size
    // (2b) thread id
    // (2b) allocated object's class name index
    // (1b) stack depth
    AllocRecord* record = &recent_allocation_records_[idx];
    size_t stack_depth = record->GetDepth();
    JDWP::Append4BE(bytes, record->byte_count);
    JDWP::Append2BE(bytes, record->thin_lock_id);
    kh.ChangeClass(record->type);
    JDWP::Append2BE(bytes, class_names.IndexOf(kh.GetDescriptor()));
    JDWP::Append1BE(bytes, stack_depth);

    MethodHelper mh;
    for (size_t stack_frame = 0; stack_frame < stack_depth; ++stack_frame) {
      // For each stack frame:
      // (2b) method's class name
      // (2b) method name
      // (2b) method source file
      // (2b) line number, clipped to 32767; -2 if native; -1 if no source
      mh.ChangeMethod(record->stack[stack_frame].method);
      JDWP::Append2BE(bytes, class_names.IndexOf(mh.GetDeclaringClassDescriptor()));
      JDWP::Append2BE(bytes, method_names.IndexOf(mh.GetName()));
      JDWP::Append2BE(bytes, filenames.IndexOf(mh.GetDeclaringClassSourceFile()));
      JDWP::Append2BE(bytes, record->stack[stack_frame].LineNumber());
    }

    idx = (idx + 1) & (kNumAllocRecords-1);
  }

  // (xb) class name strings
  // (xb) method name strings
  // (xb) source file strings
  JDWP::Set4BE(&bytes[string_table_offset], bytes.size());
  class_names.WriteTo(bytes);
  method_names.WriteTo(bytes);
  filenames.WriteTo(bytes);

  JNIEnv* env = Thread::Current()->GetJniEnv();
  jbyteArray result = env->NewByteArray(bytes.size());
  if (result != NULL) {
    env->SetByteArrayRegion(result, 0, bytes.size(), reinterpret_cast<const jbyte*>(&bytes[0]));
  }
  return result;
}

}  // namespace art
