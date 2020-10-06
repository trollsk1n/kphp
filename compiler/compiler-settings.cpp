#include "compiler/compiler-settings.h"

#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <openssl/sha.h>
#include <sstream>
#include <thread>
#include <unistd.h>

#include "common/algorithms/contains.h"
#include "common/algorithms/find.h"
#include "common/version-string.h"
#include "common/wrappers/fmt_format.h"

#include "compiler/stage.h"
#include "compiler/utils/string-utils.h"

void KphpRawOption::init(vk::string_view long_option, char short_option, vk::string_view env,
                         std::string default_value, std::vector<std::string> choices) noexcept {
  env_var_.assign(env.begin(), env.end());
  if (char *val = getenv(env_var_.c_str())) {
    raw_option_arg_ = val;
  } else {
    raw_option_arg_ = std::move(default_value);
  }

  cmd_option_full_name_.assign("--").append(long_option.begin(), long_option.end());
  if (short_option) {
    cmd_option_full_name_.append("/-").append(1, short_option);
  }
  cmd_option_full_name_.append(" [").append(env_var_).append("]");
  choices_ = std::move(choices);
}

void KphpRawOption::substitute_depends(const KphpRawOption &other) noexcept {
  std::string pattern_to_replace = "${" + other.env_var_ + "}";
  raw_option_arg_ = vk::replace_all(raw_option_arg_, pattern_to_replace, other.raw_option_arg_);
}

void KphpRawOption::verify_arg_value() const {
  if (!choices_.empty() && !vk::contains(choices_, raw_option_arg_)) {
    throw_param_exception("choose from " + vk::join(choices_, ", "));
  }
}

void KphpRawOption::throw_param_exception(const std::string &reason) const {
  throw std::runtime_error{"Can't parse " + cmd_option_full_name_ + " option: " + reason};
}

template<>
void KphpOption<std::string>::dump_option(std::ostream &out) const noexcept {
  out << value_;
}

template<>
void KphpOption<uint64_t>::dump_option(std::ostream &out) const noexcept {
  out << value_;
}

template<>
void KphpOption<bool>::dump_option(std::ostream &out) const noexcept {
  out << (value_ ? "true" : "false");
}

template<>
void KphpOption<std::vector<std::string>>::dump_option(std::ostream &out) const noexcept {
  out << vk::join(value_, ", ");
}

template<>
void KphpOption<std::string>::parse_arg_value() noexcept {
  // Don't move it, it can be used later
  value_ = raw_option_arg_;
}

template<>
void KphpOption<uint64_t>::parse_arg_value() noexcept {
  if (raw_option_arg_.empty()) {
    value_ = 0;
  } else {
    try {
      value_ = std::stoul(raw_option_arg_);
    } catch (...) {
      throw_param_exception("unsigned integer is expected");
    }
  }
}

template<>
void KphpOption<bool>::parse_arg_value() noexcept {
  if (vk::none_of_equal(raw_option_arg_, "1", "0", "")) {
    throw_param_exception("'0' or '1' are expected");
  }
  value_ = raw_option_arg_ == "1";
}

template<>
void KphpOption<std::vector<std::string>>::parse_arg_value() noexcept {
  value_ = split(raw_option_arg_, ':');
}

namespace {
static void as_dir(std::string &path) {
  if (path.empty()) {
    return;
  }
  auto full_path = get_full_path(path);
  if (!full_path.empty()) {
    path = std::move(full_path);
  }
  if (path.back() != '/') {
    path += "/";
  }
  if (path.front() != '/') {
    path.insert(0, "./");
  }
}
} // namespace

std::string CompilerSettings::get_home() noexcept {
  const char *home = getenv("HOME");
  kphp_assert(home);
  std::string home_str = home;
  as_dir(home_str);
  return home_str;
}

void CompilerSettings::option_as_dir(KphpOption<std::string> &path_option) noexcept {
  as_dir(path_option.value_);
}

bool CompilerSettings::is_static_lib_mode() const {
  return mode.get() == "lib";
}

std::string CompilerSettings::get_version() const {
  return override_kphp_version.get().empty() ? get_version_string() : override_kphp_version.get();
}

void CompilerSettings::update_cxx_flags_sha256() {
  SHA256_CTX sha256;
  SHA256_Init(&sha256);

  auto cxx_flags_full = cxx.get() + cxx_flags.get() + debug_level.get();
  SHA256_Update(&sha256, cxx_flags_full.c_str(), cxx_flags_full.size());

  unsigned char hash[SHA256_DIGEST_LENGTH] = {0};
  SHA256_Final(hash, &sha256);

  char hash_str[SHA256_DIGEST_LENGTH * 2 + 1] = {0};
  for (size_t i = 0; i < SHA256_DIGEST_LENGTH; i++) {
    fmt_format_to(hash_str + (i * 2), "{:02x}", hash[i]);
  }
  cxx_flags_sha256.value_.assign(hash_str, SHA256_DIGEST_LENGTH * 2);
}

void CompilerSettings::init() {
  option_as_dir(kphp_src_path);

  if (is_static_lib_mode()) {
    if (main_files.get().size() > 1) {
      throw std::runtime_error{"Multiple main directories are forbidden for static lib mode"};
    }
    if (!tl_schema_file.get().empty()) {
      throw std::runtime_error{"Option " + tl_schema_file.get_option_full_name() + " is forbidden for static lib mode"};
    }
    std::string lib_dir = get_full_path(main_files.get().back());
    std::size_t last_slash = lib_dir.rfind('/');
    if (last_slash == std::string::npos) {
      throw std::runtime_error{"Bad lib directory"};
    }
    static_lib_name.value_ = lib_dir.substr(last_slash + 1);
    if (static_lib_name.get().empty()) {
      throw std::runtime_error{"Empty static lib name"};
    }
    as_dir(lib_dir);
    includes.value_.emplace_back(lib_dir + "php/");
    if (static_lib_out_dir.get().empty()) {
      static_lib_out_dir.value_ = lib_dir + "lib/";
    }

    option_as_dir(static_lib_out_dir);
    main_files.value_.back().assign(lib_dir).append("/php/index.php");
  } else if (!static_lib_out_dir.get().empty()) {
    throw std::runtime_error{"Option " + static_lib_out_dir.get_option_full_name() + " is allowed only for static lib mode"};
  }

  if (!jobs_count.get()) {
    jobs_count.value_ = std::max(std::thread::hardware_concurrency(), 1U);
  }
  if (!threads_count.get()) {
    threads_count.value_ = std::max(std::thread::hardware_concurrency(), 1U);
  }

  for (string &include : includes.value_) {
    as_dir(include);
  }

  if (colorize.get() == "auto") {
    color_ = auto_colored;
  } else if (colorize.get() == "no") {
    color_ = not_colored;
  } else if (colorize.get() == "yes") {
    color_ = colored;
  } else {
    kphp_assert(0);
  }


  // TODO REMOVE IT!
  if (const char *deprecated_cxx = getenv("CXX")) {
    cxx.value_ = deprecated_cxx;
  }

  // TODO REMOVE IT!
  if (const char *deprecated_cxx_flags = getenv("CXXFLAGS")) {
    extra_cxx_flags.value_ = deprecated_cxx_flags;
  }

  // TODO REMOVE IT!
  if (const char *deprecated_ld_flags = getenv("LDFLAGS")) {
    extra_ld_flags.value_ = deprecated_ld_flags;
  }

  remove_extra_spaces(extra_cxx_flags.value_);
  std::stringstream ss;
  ss << extra_cxx_flags.get();
  ss << " -iquote" << kphp_src_path.get() << " -iquote" << kphp_src_path.get() << "PHP/";
  ss << " -Wall -fwrapv -Wno-parentheses -Wno-trigraphs";
  ss << " -fno-strict-aliasing -fno-omit-frame-pointer";
  if (!no_pch.get()) {
    ss << " -Winvalid-pch -fpch-preprocess";
  }
  if (dynamic_incremental_linkage.get()) {
    ss << " -fPIC";
  }
  if (vk::contains(cxx.get(), "clang")) {
    ss << " -Wno-invalid-source-encoding";
  }
  #if __cplusplus <= 201103L
  ss << " -std=gnu++11";
  #elif __cplusplus <= 201402L
  ss << " -std=gnu++14";
  #elif __cplusplus <= 201703L
  ss << " -std=gnu++17";
  #elif __cplusplus <= 202002L
  ss << " -std=gnu++20";
  #else
    #error unsupported __cplusplus value
  #endif

  cxx_flags.value_ = ss.str();

  update_cxx_flags_sha256();
  runtime_sha256.value_ = read_runtime_sha256_file(runtime_sha256_file.get());

  incremental_linker.value_ = dynamic_incremental_linkage.get() ? cxx.get() : "ld";
  incremental_linker_flags.value_ = dynamic_incremental_linkage.get() ? "-shared" : "-r";

  remove_extra_spaces(extra_ld_flags.value_);
  ld_flags.value_ = extra_ld_flags.get() + " -lm -lz -lpthread -lrt -lcrypto -lpcre -lre2 -lyaml-cpp -lh3 -rdynamic";

  option_as_dir(dest_dir);

  dest_cpp_dir.value_ = dest_dir.get() + "kphp/";
  dest_objs_dir.value_ = dest_dir.get() + "objs/";
  binary_path.value_ = dest_dir.get() + mode.get();
  cxx_flags.value_ += " -iquote" + dest_cpp_dir.get();

  tl_namespace_prefix.value_ = "VK\\TL\\";
  tl_classname_prefix.value_ = "C$VK$TL$";
}

std::string CompilerSettings::read_runtime_sha256_file(const std::string &filename) {
  std::ifstream runtime_sha256_file(filename.c_str());
  kphp_error(runtime_sha256_file,
             fmt_format("Can't open runtime sha256 file '{}'", filename));

  constexpr std::streamsize SHA256_LEN = 64;
  char runtime_sha256[SHA256_LEN] = {0};
  runtime_sha256_file.read(runtime_sha256, SHA256_LEN);
  kphp_error(runtime_sha256_file.gcount() == SHA256_LEN,
             fmt_format("Can't read runtime sha256 from file '{}'", filename));
  return std::string(runtime_sha256, runtime_sha256 + SHA256_LEN);
}

CompilerSettings::color_settings CompilerSettings::get_color_settings() const {
  return color_;
}
