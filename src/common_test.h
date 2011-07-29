// Copyright 2011 Google Inc. All Rights Reserved.

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "base64.h"
#include "heap.h"
#include "thread.h"
#include "stringprintf.h"
#include "class_linker.h"
#include "dex_file.h"

#include "gtest/gtest.h"

namespace art {

// package java.lang;
// public class Object {}

// package java.lang;
// public class Class {}
//
// package java.lang.reflect;
// public class Field {}
//
// package java.lang.reflect;
// public class Method {}
//
// package java.lang;
// public class String {
//   char[] value;
//   int hashCode;
//   int offset;
//   int count;
// }
//
// package java.lang;
// public interface Cloneable {}
//
// package java.io;
// public interface Serializable {}
static const char kJavaLangDex[] =
  "ZGV4CjAzNQDgopvWPbyCTsLOzSYO4VPqS6aRqcz6ZQu0BAAAcAAAAHhWNBIAAAAAAAAAACAEAAAW"
  "AAAAcAAAAAoAAADIAAAAAQAAAPAAAAAEAAAA/AAAAAUAAAAcAQAABwAAAEQBAACQAgAAJAIAAJgC"
  "AACgAgAArAIAALwCAADIAgAAywIAAOMCAAD2AgAADQMAACEDAAA1AwAAUAMAAGwDAAB5AwAAhgMA"
  "AJkDAACmAwAAqQMAAK0DAAC0AwAAvgMAAMYDAAAEAAAABQAAAAYAAAAHAAAACAAAAAkAAAAKAAAA"
  "CwAAABAAAAARAAAAEAAAAAgAAAAAAAAABQAAABIAAAAFAAAAEwAAAAUAAAAUAAAABQAJABUAAAAC"
  "AAAAAAAAAAQAAAAAAAAABQAAAAAAAAAGAAAAAAAAAAcAAAAAAAAABAAAAAEAAAD/////AAAAAA0A"
  "AAAAAAAA5gMAAAAAAAABAAAAAQYAAAQAAAAAAAAADgAAAAAAAAAAAAAAAAAAAAIAAAABAAAABAAA"
  "AAAAAAABAAAAAAAAAPADAAAAAAAAAwAAAAEGAAAEAAAAAAAAAAIAAAAAAAAAAAAAAAAAAAAFAAAA"
  "AQAAAAQAAAAAAAAADwAAAAAAAAD6AwAAAAAAAAYAAAABAAAABAAAAAAAAAADAAAAAAAAAAwEAAAA"
  "AAAABwAAAAEAAAAEAAAAAAAAAAwAAAAAAAAAFgQAAAAAAAABAAEAAAAAAM0DAAABAAAADgAAAAEA"
  "AQABAAAA0gMAAAQAAABwEAEAAAAOAAEAAQABAAAA1wMAAAQAAABwEAEAAAAOAAEAAQABAAAA3AMA"
  "AAQAAABwEAEAAAAOAAEAAQABAAAA4QMAAAQAAABwEAEAAAAOAAY8aW5pdD4ACkNsYXNzLmphdmEA"
  "DkNsb25lYWJsZS5qYXZhAApGaWVsZC5qYXZhAAFJABZMamF2YS9pby9TZXJpYWxpemFibGU7ABFM"
  "amF2YS9sYW5nL0NsYXNzOwAVTGphdmEvbGFuZy9DbG9uZWFibGU7ABJMamF2YS9sYW5nL09iamVj"
  "dDsAEkxqYXZhL2xhbmcvU3RyaW5nOwAZTGphdmEvbGFuZy9yZWZsZWN0L0ZpZWxkOwAaTGphdmEv"
  "bGFuZy9yZWZsZWN0L01ldGhvZDsAC01ldGhvZC5qYXZhAAtPYmplY3QuamF2YQARU2VyaWFsaXph"
  "YmxlLmphdmEAC1N0cmluZy5qYXZhAAFWAAJbQwAFY291bnQACGhhc2hDb2RlAAZvZmZzZXQABXZh"
  "bHVlAAMABw4ABQAHDgAFAAcOAAUABw4ABQAHDgAAAAEAAYGABKQEAAABAACBgAS4BAAEAQAAAAEA"
  "AQABAAKBgATQBAAAAQADgYAE6AQAAAEABIGABIAFDAAAAAAAAAABAAAAAAAAAAEAAAAWAAAAcAAA"
  "AAIAAAAKAAAAyAAAAAMAAAABAAAA8AAAAAQAAAAEAAAA/AAAAAUAAAAFAAAAHAEAAAYAAAAHAAAA"
  "RAEAAAEgAAAFAAAAJAIAAAIgAAAWAAAAmAIAAAMgAAAFAAAAzQMAAAAgAAAFAAAA5gMAAAAQAAAB"
  "AAAAIAQAAA==";

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

static inline DexFile* OpenDexFileBase64(const char* base64) {
  CHECK(base64 != NULL);
  size_t length;
  byte* dex_bytes = DecodeBase64(base64, &length);
  CHECK(dex_bytes != NULL);
  DexFile* dex_file = DexFile::OpenPtr(dex_bytes, length);
  CHECK(dex_file != NULL);
  return dex_file;
}

class RuntimeTest : public testing::Test {
 protected:
  virtual void SetUp() {
    is_host_ = getenv("ANDROID_BUILD_TOP") != NULL;

    android_data_.reset(strdup(is_host_ ? "/tmp/art-data-XXXXXX" : "/sdcard/art-data-XXXXXX"));
    ASSERT_TRUE(android_data_ != NULL);
    const char* android_data_modified = mkdtemp(android_data_.get());
    // note that mkdtemp side effects android_data_ as well
    ASSERT_TRUE(android_data_modified != NULL);
    setenv("ANDROID_DATA", android_data_modified, 1);
    art_cache_.append(android_data_.get());
    art_cache_.append("/art-cache");
    int mkdir_result = mkdir(art_cache_.c_str(), 0700);
    ASSERT_EQ(mkdir_result, 0);

    java_lang_dex_file_.reset(OpenDexFileBase64(kJavaLangDex));

    std::vector<DexFile*> boot_class_path;
    boot_class_path.push_back(java_lang_dex_file_.get());

    runtime_.reset(Runtime::Create(boot_class_path));
    ASSERT_TRUE(runtime_ != NULL);
    class_linker_ = runtime_->GetClassLinker();
  }

  virtual void TearDown() {
    const char* android_data = getenv("ANDROID_DATA");
    ASSERT_TRUE(android_data != NULL);
    DIR* dir = opendir(art_cache_.c_str());
    ASSERT_TRUE(dir != NULL);
    while (true) {
      struct dirent entry;
      struct dirent* entry_ptr;
      int readdir_result = readdir_r(dir, &entry, &entry_ptr);
      ASSERT_EQ(0, readdir_result);
      if (entry_ptr == NULL) {
        break;
      }
      if ((strcmp(entry_ptr->d_name, ".") == 0) || (strcmp(entry_ptr->d_name, "..") == 0)) {
        continue;
      }
      std::string filename(art_cache_);
      filename.push_back('/');
      filename.append(entry_ptr->d_name);
      int unlink_result = unlink(filename.c_str());
      ASSERT_EQ(0, unlink_result);
    }
    closedir(dir);
    int rmdir_cache_result = rmdir(art_cache_.c_str());
    ASSERT_EQ(0, rmdir_cache_result);
    int rmdir_data_result = rmdir(android_data_.get());
    ASSERT_EQ(0, rmdir_data_result);
  }

  std::string GetLibCoreDexFileName() {
    if (is_host_) {
      const char* host_dir = getenv("ANDROID_HOST_OUT");
      CHECK(host_dir != NULL);
      return StringPrintf("%s/framework/core-hostdex.jar", host_dir);
    }
    return std::string("/system/framework/core.jar");
  }

  DexFile* GetLibCoreDex() {
    std::string libcore_dex_file_name = GetLibCoreDexFileName();
    return DexFile::OpenZip(libcore_dex_file_name.c_str());
  }

  void UseLibCoreDex() {
    delete runtime_.release();
    java_lang_dex_file_.reset(GetLibCoreDex());

    std::vector<DexFile*> boot_class_path;
    boot_class_path.push_back(java_lang_dex_file_.get());

    runtime_.reset(Runtime::Create(boot_class_path));
    ASSERT_TRUE(runtime_ != NULL);
    class_linker_ = runtime_->GetClassLinker();
  }

  bool is_host_;
  scoped_ptr_malloc<char> android_data_;
  std::string art_cache_;
  scoped_ptr<DexFile> java_lang_dex_file_;
  scoped_ptr<Runtime> runtime_;
  ClassLinker* class_linker_;
};

}  // namespace art
