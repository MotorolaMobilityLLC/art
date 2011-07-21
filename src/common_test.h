// Copyright 2011 Google Inc. All Rights Reserved.

#include "base64.h"
#include "heap.h"
#include "thread.h"
#include "class_linker.h"
#include "dex_file.h"

#include "gtest/gtest.h"

namespace art {

// package java.lang;
// public class Object {}
//
// package java.lang;
// public interface Cloneable {}
//
// package java.io;
// public interface Serializable {}
static const char kJavaLangDex[] =
  "ZGV4CjAzNQAdS5NB20pfz7Z3u9Jh2IVuB3Hxe6BzR9FAAgAAcAAAAHhWNBIAAAAAAAAAALgBAAAI"
  "AAAAcAAAAAQAAACQAAAAAQAAAKAAAAAAAAAAAAAAAAEAAACsAAAAAwAAALQAAAAsAQAAFAEAACgB"
  "AAAwAQAAQAEAAFgBAABvAQAAgwEAAJABAACjAQAAAgAAAAMAAAAEAAAABwAAAAcAAAADAAAAAAAA"
  "AAIAAAAAAAAAAgAAAAEAAAD/////AAAAAAUAAAAAAAAAqwEAAAAAAAAAAAAAAQYAAAIAAAAAAAAA"
  "BgAAAAAAAAAAAAAAAAAAAAEAAAABBgAAAgAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAQABAAAAAACm"
  "AQAAAQAAAA4AAAAGPGluaXQ+AA5DbG9uZWFibGUuamF2YQAWTGphdmEvaW8vU2VyaWFsaXphYmxl"
  "OwAVTGphdmEvbGFuZy9DbG9uZWFibGU7ABJMamF2YS9sYW5nL09iamVjdDsAC09iamVjdC5qYXZh"
  "ABFTZXJpYWxpemFibGUuamF2YQABVgADAAcOAAAAAQAAgYAElAIAAAALAAAAAAAAAAEAAAAAAAAA"
  "AQAAAAgAAABwAAAAAgAAAAQAAACQAAAAAwAAAAEAAACgAAAABQAAAAEAAACsAAAABgAAAAMAAAC0"
  "AAAAASAAAAEAAAAUAQAAAiAAAAgAAAAoAQAAAyAAAAEAAACmAQAAACAAAAEAAACrAQAAABAAAAEA"
  "AAC4AQAA";

// package java.lang;
// public class Object {}
//
// class MyClass {}
static const char kMyClassDex[] =
  "ZGV4CjAzNQA5Nm9IrCVm91COwepff7LhIE23GZIxGjgIAgAAcAAAAHhWNBIAAAAAAAAAAIABAAAG"
  "AAAAcAAAAAMAAACIAAAAAQAAAJQAAAAAAAAAAAAAAAIAAACgAAAAAgAAALAAAAAYAQAA8AAAABwB"
  "AAAkAQAALwEAAEMBAABRAQAAXgEAAAEAAAACAAAABQAAAAUAAAACAAAAAAAAAAAAAAAAAAAAAQAA"
  "AAAAAAABAAAAAQAAAP////8AAAAABAAAAAAAAABrAQAAAAAAAAAAAAAAAAAAAQAAAAAAAAADAAAA"
  "AAAAAHUBAAAAAAAAAQABAAAAAABhAQAAAQAAAA4AAAABAAEAAQAAAGYBAAAEAAAAcBABAAAADgAG"
  "PGluaXQ+AAlMTXlDbGFzczsAEkxqYXZhL2xhbmcvT2JqZWN0OwAMTXlDbGFzcy5qYXZhAAtPYmpl"
  "Y3QuamF2YQABVgACAAcOAAUABw4AAAABAAGBgATwAQAAAQAAgIAEhAIACwAAAAAAAAABAAAAAAAA"
  "AAEAAAAGAAAAcAAAAAIAAAADAAAAiAAAAAMAAAABAAAAlAAAAAUAAAACAAAAoAAAAAYAAAACAAAA"
  "sAAAAAEgAAACAAAA8AAAAAIgAAAGAAAAHAEAAAMgAAACAAAAYQEAAAAgAAACAAAAawEAAAAQAAAB"
  "AAAAgAEAAA==";

// class Nested {
//     class Inner {
//     }
// }
static const char kNestedDex[] =
  "ZGV4CjAzNQAQedgAe7gM1B/WHsWJ6L7lGAISGC7yjD2IAwAAcAAAAHhWNBIAAAAAAAAAAMQCAAAP"
  "AAAAcAAAAAcAAACsAAAAAgAAAMgAAAABAAAA4AAAAAMAAADoAAAAAgAAAAABAABIAgAAQAEAAK4B"
  "AAC2AQAAvQEAAM0BAADXAQAA+wEAABsCAAA+AgAAUgIAAF8CAABiAgAAZgIAAHMCAAB5AgAAgQIA"
  "AAIAAAADAAAABAAAAAUAAAAGAAAABwAAAAkAAAAJAAAABgAAAAAAAAAKAAAABgAAAKgBAAAAAAEA"
  "DQAAAAAAAQAAAAAAAQAAAAAAAAAFAAAAAAAAAAAAAAAAAAAABQAAAAAAAAAIAAAAiAEAAKsCAAAA"
  "AAAAAQAAAAAAAAAFAAAAAAAAAAgAAACYAQAAuAIAAAAAAAACAAAAlAIAAJoCAAABAAAAowIAAAIA"
  "AgABAAAAiAIAAAYAAABbAQAAcBACAAAADgABAAEAAQAAAI4CAAAEAAAAcBACAAAADgBAAQAAAAAA"
  "AAAAAAAAAAAATAEAAAAAAAAAAAAAAAAAAAEAAAABAAY8aW5pdD4ABUlubmVyAA5MTmVzdGVkJElu"
  "bmVyOwAITE5lc3RlZDsAIkxkYWx2aWsvYW5ub3RhdGlvbi9FbmNsb3NpbmdDbGFzczsAHkxkYWx2"
  "aWsvYW5ub3RhdGlvbi9Jbm5lckNsYXNzOwAhTGRhbHZpay9hbm5vdGF0aW9uL01lbWJlckNsYXNz"
  "ZXM7ABJMamF2YS9sYW5nL09iamVjdDsAC05lc3RlZC5qYXZhAAFWAAJWTAALYWNjZXNzRmxhZ3MA"
  "BG5hbWUABnRoaXMkMAAFdmFsdWUAAgEABw4AAQAHDjwAAgIBDhgBAgMCCwQADBcBAgQBDhwBGAAA"
  "AQEAAJAgAICABNQCAAABAAGAgATwAgAAEAAAAAAAAAABAAAAAAAAAAEAAAAPAAAAcAAAAAIAAAAH"
  "AAAArAAAAAMAAAACAAAAyAAAAAQAAAABAAAA4AAAAAUAAAADAAAA6AAAAAYAAAACAAAAAAEAAAMQ"
  "AAACAAAAQAEAAAEgAAACAAAAVAEAAAYgAAACAAAAiAEAAAEQAAABAAAAqAEAAAIgAAAPAAAArgEA"
  "AAMgAAACAAAAiAIAAAQgAAADAAAAlAIAAAAgAAACAAAAqwIAAAAQAAABAAAAxAIAAA==";

// class ProtoCompare {
//     int m1(short x, int y, long z) { return x + y + (int)z; }
//     int m2(short x, int y, long z) { return x + y + (int)z; }
//     int m3(long x, int y, short z) { return (int)x + y + z; }
//     long m4(long x, int y, short z) { return x + y + z; }
// }
static const char kProtoCompareDex[] =
  "ZGV4CjAzNQBLUetu+TVZ8gsYsCOFoij7ecsHaGSEGA8gAwAAcAAAAHhWNBIAAAAAAAAAAIwCAAAP"
  "AAAAcAAAAAYAAACsAAAABAAAAMQAAAAAAAAAAAAAAAYAAAD0AAAAAQAAACQBAADcAQAARAEAAN4B"
  "AADmAQAA6QEAAO8BAAD1AQAA+AEAAP4BAAAOAgAAIgIAADUCAAA4AgAAOwIAAD8CAABDAgAARwIA"
  "AAEAAAAEAAAABgAAAAcAAAAJAAAACgAAAAIAAAAAAAAAyAEAAAMAAAAAAAAA1AEAAAUAAAABAAAA"
  "yAEAAAoAAAAFAAAAAAAAAAIAAwAAAAAAAgABAAsAAAACAAEADAAAAAIAAAANAAAAAgACAA4AAAAD"
  "AAMAAAAAAAIAAAAAAAAAAwAAAAAAAAAIAAAAAAAAAHACAAAAAAAAAQABAAEAAABLAgAABAAAAHAQ"
  "BQAAAA4ABwAFAAAAAABQAgAABQAAAJAAAwSEUbAQDwAAAAcABQAAAAAAWAIAAAUAAACQAAMEhFGw"
  "EA8AAAAGAAUAAAAAAGACAAAEAAAAhCCwQLBQDwAJAAUAAAAAAGgCAAAFAAAAgXC7UIGCuyAQAAAA"
  "AwAAAAEAAAAEAAAAAwAAAAQAAAABAAY8aW5pdD4AAUkABElKSVMABElTSUoAAUoABEpKSVMADkxQ"
  "cm90b0NvbXBhcmU7ABJMamF2YS9sYW5nL09iamVjdDsAEVByb3RvQ29tcGFyZS5qYXZhAAFTAAFW"
  "AAJtMQACbTIAAm0zAAJtNAABAAcOAAIDAAAABw4AAwMAAAAHDgAEAwAAAAcOAAUDAAAABw4AAAAB"
  "BACAgATEAgEA3AIBAPgCAQCUAwEArAMAAAwAAAAAAAAAAQAAAAAAAAABAAAADwAAAHAAAAACAAAA"
  "BgAAAKwAAAADAAAABAAAAMQAAAAFAAAABgAAAPQAAAAGAAAAAQAAACQBAAABIAAABQAAAEQBAAAB"
  "EAAAAgAAAMgBAAACIAAADwAAAN4BAAADIAAABQAAAEsCAAAAIAAAAQAAAHACAAAAEAAAAQAAAIwC"
  "AAA=";

// class ProtoCompare2 {
//     int m1(short x, int y, long z) { return x + y + (int)z; }
//     int m2(short x, int y, long z) { return x + y + (int)z; }
//     int m3(long x, int y, short z) { return (int)x + y + z; }
//     long m4(long x, int y, short z) { return x + y + z; }
// }
static const char kProtoCompare2Dex[] =
  "ZGV4CjAzNQDVUXj687EpyTTDJZEZPA8dEYnDlm0Ir6YgAwAAcAAAAHhWNBIAAAAAAAAAAIwCAAAP"
  "AAAAcAAAAAYAAACsAAAABAAAAMQAAAAAAAAAAAAAAAYAAAD0AAAAAQAAACQBAADcAQAARAEAAN4B"
  "AADmAQAA6QEAAO8BAAD1AQAA+AEAAP4BAAAPAgAAIwIAADcCAAA6AgAAPQIAAEECAABFAgAASQIA"
  "AAEAAAAEAAAABgAAAAcAAAAJAAAACgAAAAIAAAAAAAAAyAEAAAMAAAAAAAAA1AEAAAUAAAABAAAA"
  "yAEAAAoAAAAFAAAAAAAAAAIAAwAAAAAAAgABAAsAAAACAAEADAAAAAIAAAANAAAAAgACAA4AAAAD"
  "AAMAAAAAAAIAAAAAAAAAAwAAAAAAAAAIAAAAAAAAAHICAAAAAAAAAQABAAEAAABNAgAABAAAAHAQ"
  "BQAAAA4ABwAFAAAAAABSAgAABQAAAJAAAwSEUbAQDwAAAAcABQAAAAAAWgIAAAUAAACQAAMEhFGw"
  "EA8AAAAGAAUAAAAAAGICAAAEAAAAhCCwQLBQDwAJAAUAAAAAAGoCAAAFAAAAgXC7UIGCuyAQAAAA"
  "AwAAAAEAAAAEAAAAAwAAAAQAAAABAAY8aW5pdD4AAUkABElKSVMABElTSUoAAUoABEpKSVMAD0xQ"
  "cm90b0NvbXBhcmUyOwASTGphdmEvbGFuZy9PYmplY3Q7ABJQcm90b0NvbXBhcmUyLmphdmEAAVMA"
  "AVYAAm0xAAJtMgACbTMAAm00AAEABw4AAgMAAAAHDgADAwAAAAcOAAQDAAAABw4ABQMAAAAHDgAA"
  "AAEEAICABMQCAQDcAgEA+AIBAJQDAQCsAwwAAAAAAAAAAQAAAAAAAAABAAAADwAAAHAAAAACAAAA"
  "BgAAAKwAAAADAAAABAAAAMQAAAAFAAAABgAAAPQAAAAGAAAAAQAAACQBAAABIAAABQAAAEQBAAAB"
  "EAAAAgAAAMgBAAACIAAADwAAAN4BAAADIAAABQAAAE0CAAAAIAAAAQAAAHICAAAAEAAAAQAAAIwC"
  "AAA=";

// javac MyClass.java && dx --dex --output=MyClass.dex
//   --core-library MyClass.class java/lang/Object.class && base64 MyClass.dex
// package java.lang;
// public class Object {}
// class MyClass {
//   native void foo();
//   native int fooI(int x);
//   native int fooII(int x, int y);
//   native double fooDD(double x, double y);
//   native Object fooIOO(int x, Object y, Object z);
//   static native Object fooSIOO(int x, Object y, Object z);
//   static synchronized native Object fooSSIOO(int x, Object y, Object z);
// }
static const char kMyClassNativesDex[] =
  "ZGV4CjAzNQA4WWrpXgdlkoTHR8Yubx4LJO4HbGsX1p1EAwAAcAAAAHhWNBIAAAAAAAAAALACAAAT"
  "AAAAcAAAAAUAAAC8AAAABQAAANAAAAAAAAAAAAAAAAkAAAAMAQAAAgAAAFQBAACwAQAAlAEAAOIB"
  "AADqAQAA7QEAAPIBAAD1AQAA+QEAAP4BAAAEAgAADwIAACMCAAAxAgAAPgIAAEECAABGAgAATQIA"
  "AFMCAABaAgAAYgIAAGsCAAABAAAAAwAAAAcAAAAIAAAACwAAAAIAAAAAAAAAwAEAAAQAAAABAAAA"
  "yAEAAAUAAAABAAAA0AEAAAYAAAADAAAA2AEAAAsAAAAEAAAAAAAAAAIABAAAAAAAAgAEAAwAAAAC"
  "AAAADQAAAAIAAQAOAAAAAgACAA8AAAACAAMAEAAAAAIAAwARAAAAAgADABIAAAADAAQAAAAAAAMA"
  "AAABAAAA/////wAAAAAKAAAAAAAAAH8CAAAAAAAAAgAAAAAAAAADAAAAAAAAAAkAAAAAAAAAiQIA"
  "AAAAAAABAAEAAAAAAHUCAAABAAAADgAAAAEAAQABAAAAegIAAAQAAABwEAgAAAAOAAIAAAAAAAAA"
  "AQAAAAEAAAACAAAAAQABAAMAAAABAAMAAwAGPGluaXQ+AAFEAANEREQAAUkAAklJAANJSUkABExJ"
  "TEwACUxNeUNsYXNzOwASTGphdmEvbGFuZy9PYmplY3Q7AAxNeUNsYXNzLmphdmEAC09iamVjdC5q"
  "YXZhAAFWAANmb28ABWZvb0REAARmb29JAAVmb29JSQAGZm9vSU9PAAdmb29TSU9PAAhmb29TU0lP"
  "TwADAAcOAAEABw4AAAABAAiBgASUAwAAAwUAgIAEqAMGiAIAAaiCCAABgAIAAYACAAGAAgABgAIA"
  "AYACAAwAAAAAAAAAAQAAAAAAAAABAAAAEwAAAHAAAAACAAAABQAAALwAAAADAAAABQAAANAAAAAF"
  "AAAACQAAAAwBAAAGAAAAAgAAAFQBAAABIAAAAgAAAJQBAAABEAAABAAAAMABAAACIAAAEwAAAOIB"
  "AAADIAAAAgAAAHUCAAAAIAAAAgAAAH8CAAAAEAAAAQAAALACAAA=";

static inline RawDexFile* OpenRawDexFileBase64(const char* base64) {
  CHECK(base64 != NULL);
  size_t length;
  byte* dex_file = DecodeBase64(base64, &length);
  CHECK(dex_file != NULL);
  RawDexFile* raw_dex_file = RawDexFile::OpenPtr(dex_file, length);
  CHECK(raw_dex_file != NULL);
  return raw_dex_file;
}

class RuntimeTest : public testing::Test {
 protected:
  virtual void SetUp() {
    ASSERT_TRUE(Thread::Init());
    ASSERT_TRUE(Thread::Attach() != NULL);
    ASSERT_TRUE(Heap::Init());

    java_lang_raw_dex_file_.reset(OpenRawDexFileBase64(kJavaLangDex));

    std::vector<RawDexFile*> boot_class_path;
    boot_class_path.push_back(java_lang_raw_dex_file_.get());

    class_linker_.reset(ClassLinker::Create(boot_class_path));
    CHECK(class_linker_ != NULL);
  }

  virtual void TearDown() {
    Heap::Destroy();
  }

  scoped_ptr<RawDexFile> java_lang_raw_dex_file_;
  scoped_ptr<ClassLinker> class_linker_;
};

}  // namespace art
