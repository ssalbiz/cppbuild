#define PACKAGE "cppbuild"
#define PACKAGE_VERSION 0.1

#include <cstring>
#include <iostream>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <cstdlib>
#include <dirent.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <bfd.h>

#include "scope.h"

using std::queue;
using std::string;
using std::unordered_map;
using std::unordered_set;
using std::vector;

static const char* kMainSymName = "_main";
static const int kCommandScratch = 32;

inline DIR* OpenDirOrDie(const string& dirpath) {
  auto* dir_handle = opendir(dirpath.c_str());
  if (dir_handle == nullptr) {
    std::cerr << "Could not open directory:" << dirpath << std::endl;
    exit(1);
  }
  return dir_handle;
}

template <class Func>
inline void VisitDirEntries(DIR* dir_handle, Func f) {
  auto* dir_entry = readdir(dir_handle);
  while (dir_entry != nullptr) {
    f(dir_entry);
    dir_entry = readdir(dir_handle);
  }
}

inline bool ends_with(const string& in, const string& suffix) {
  return in.size() >= suffix.size()
    && in.compare(in.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline string GetCurrentHomeDir() {
  const char* homedir = getenv("HOME");
  return homedir != nullptr
    ? string(homedir)
    : string(getpwuid(getuid())->pw_dir);
}

inline string RealPath(const std::string& path) {
  char* resolved = realpath(path.c_str(), nullptr);
  if (resolved != nullptr) {
    // std::string makes a copy.
    string real(resolved);
    free(resolved);
    return real;
  }
  return "";
}

inline string GetCppRootDir(int argc, char* argv[]) {
  string root_dir = "";
  // First try to look for commandline arg.
  if (argc > 1 && argv != nullptr) {
    // TODO.
  }

  // Then fallback to environment variable
  const char* env_dir = getenv("CPPROOT");
  if (env_dir != nullptr) {
    root_dir = RealPath(env_dir);
  }

  // Finally try $HOME/src
  if (root_dir.empty()) {
    root_dir = RealPath(GetCurrentHomeDir() + "/src/");
  }
  return root_dir;
}

inline string GetTargetOrDie(int argc, char* argv[]) {
  string target = "";
  if (argc > 1 && argv != nullptr) {
    target = argv[1];
  }
  if (target.empty()) {
    std::cerr << "Error: no target specified" << std::endl;
    exit(1);
  }
  return target;
}

bool IsSourceFile(const string& filename) {
  return ends_with(filename, ".cc");
}

bool IsHeaderFile(const string& filename) {
  return ends_with(filename, ".h")
    || ends_with(filename, ".hpp");
}

inline string& replace_last(string& haystack, const string& needle, const string& replacement) {
  auto i = haystack.rfind(needle);
  return (i == string::npos)
    ? haystack
    : haystack.replace(i, needle.size(), replacement);
}

inline string GenerateExpectedObjectFileName(string filename) {
  return replace_last(filename, ".cc", ".o");
}

inline string GenerateExpectedBinaryFileName(string filename) {
  return replace_last(filename, ".o", "");
}

inline void RunOrDie(const string& command) {
  std::cerr << "Executing: " << command << "..." << std::endl;
  int result = system(command.c_str());
  if (!WIFEXITED(result) || WEXITSTATUS(result) != 0) {
    std::cerr << "Failed to run: " << command << std::endl;
    exit(1);
  }
}

unordered_map<string, vector<string>> package_to_file;
unordered_map<string, vector<string>> package_to_main;
unordered_map<string, vector<string>> file_to_undefined_symbols;
unordered_map<string, string> exported_symbol_to_file;

void PopulateSymbolsOrDie(bfd* abfd, const string& package, const string& filepath) {
  char** matching = nullptr;
  if (!bfd_check_format_matches(abfd, bfd_object, &matching)) {
    std::cerr << "Error, got a not-obj bfd" << std::endl;
    exit(1);
  }
  if (matching != nullptr) {
    while (*matching) {
      std::cerr << "matching format: " << *matching++ << std::endl;
    }
  }

  long upper_bound_number_of_symbols = bfd_get_symtab_upper_bound(abfd);
  if (upper_bound_number_of_symbols < 0) {
    std::cerr << "Unable to read symbols from BFD." << std::endl;
    exit(1);
  }
  if (upper_bound_number_of_symbols == 0) {
    std::cerr << "Empty symbol table read from BFD. Continuing with build." << std::endl;
    return;
  }
  vector<asymbol*> symbol_table;
  symbol_table.resize(upper_bound_number_of_symbols);
  long number_of_symbols = bfd_canonicalize_symtab(abfd, symbol_table.data());
  std::cerr << "read " << number_of_symbols << " symbols from obj file." << std::endl;
  if (number_of_symbols < 0) {
    std::cerr << "Unable to read symbols from BFD." << std::endl;
    exit(1);
  }

  vector<string> syms_for_file;
  vector<string> undef_syms_for_file;
  bool has_main = false;
  for (long i = 0; i < number_of_symbols; ++i) {
    auto* symbol = symbol_table[i];
    bool is_undefined = bfd_is_und_section(symbol->section);
    bool is_exported =
      (symbol->flags & (BSF_GLOBAL | BSF_WEAK | BSF_GNU_UNIQUE))
      || bfd_is_com_section(symbol->section);
    if (is_undefined) {
      undef_syms_for_file.emplace_back(symbol->name);
    } else if (is_exported) {
      exported_symbol_to_file.emplace(symbol->name, filepath);
      if (string(symbol->name).find(kMainSymName) != string::npos) {
        has_main = true;
      }
    }
  }

  // Populate the global lookup tables.
  for (auto& sym : undef_syms_for_file) {
    file_to_undefined_symbols[filepath].emplace_back(sym);
  }
  package_to_file[package].emplace_back(filepath);
  if (has_main) {
    package_to_main[package].emplace_back(filepath);
  }
}

bool LoadPackage(const string& cpp_root_dir, const string& package) {
  string package_dir = cpp_root_dir + "/" + package;
  auto* package_dir_handle = OpenDirOrDie(package_dir);
  SCOPE_GUARD(closedir(package_dir_handle));
  vector<string> source_files;
  VisitDirEntries(package_dir_handle, [&](dirent* dir_entry) {
    string entry_name = dir_entry->d_name;
    if (IsSourceFile(entry_name)) {
      source_files.emplace_back(entry_name);
    }
  });

  for (auto sf : source_files) {
    string of = GenerateExpectedObjectFileName(sf);
    string sf_path = package_dir + "/" + sf;
    string of_path = package_dir + "/" + of;
    string df_path = package_dir + "/" + sf + ".d";
    // Generate prerequisites for source file.
    // FIXME: stat the .d & .cc files first to see if the prereq needs to be regenerated first.
    string command = "c++ -MF " + df_path + " -MM " + sf_path + " -MT " + of_path;
    RunOrDie(command);

    // Use implicit make rules to build the object files. If make fails, halt the build.
    command = "make -f " + df_path;
    RunOrDie(command);
    // Now that we've generated the object files, populate a symbol lookup table
    // by reading symbols from the generated object files.
    auto* bfd = bfd_openr(of_path.c_str(), nullptr);
    if (bfd == nullptr) {
      std::cerr << "Unable to read object symbol table for " << of << std::endl;
      exit(1);
    }
    SCOPE_GUARD(bfd_close(bfd));
    std::cout << "Populating symbol table for object file " << of_path << std::endl;
    PopulateSymbolsOrDie(bfd, package, of_path);
  }
  return true;
}

void DumpTable(const unordered_map<string, vector<string>>& table) {
  for (auto& entry : table) {
    std::cerr << entry.first << "->" << std::endl;
    for (auto& v : entry.second) {
      std::cerr << v << std::endl;
    }
  }
}

void LinkDeps(const string& target) {
  const auto& main_files = package_to_main[target];
  if (main_files.empty()) {
    return;
  }
  for (const auto& mf : main_files) {
    unordered_set<string> depfiles;
    queue<string> file_work_queue;
    file_work_queue.emplace(mf);
    auto command_buffer_size = mf.size();
    while (!file_work_queue.empty()) {
      const auto& file = file_work_queue.front();
      file_work_queue.pop();
      const auto& syms = file_to_undefined_symbols[file];
      for (const auto& sym : syms) {
        if (exported_symbol_to_file.count(sym)) {
          const auto& depfile = exported_symbol_to_file.at(sym);
          depfiles.emplace(depfile);
          command_buffer_size += depfile.size() + 1;
          if (!file_to_undefined_symbols[depfile].empty()) {
            file_work_queue.emplace(depfile);
          }
        }
      }
    }
    // Computed transitive closure of all files necessary to compile binary for main file.
    // Produce binary
    string binary = GenerateExpectedBinaryFileName(mf);
    command_buffer_size += binary.size();
    // FIXME: Get ldflags from somewhere? Package-specific flags or something?
    string command;
    // Reserve enough space to append all the depfiles and stuff.
    command.reserve(command_buffer_size + kCommandScratch);
    command.append("c++ -o ");
    command.append(binary);
    command.append(" ");
    command.append(mf);
    for (const auto& depfile : depfiles) {
      command.append(" ");
      command.append(depfile);
    }
    RunOrDie(command);
  }
}

int main(int argc, char* argv[]) {
  // Initialize bfd library.
  bfd_init();
  // Get project to build.
  string target = GetTargetOrDie(argc, argv);
  std::cout << "Building target: " << target << std::endl;

  string cpp_root_dir = GetCppRootDir(argc, argv);
  std::cout << "Using cpproot: " << cpp_root_dir << std::endl;

  vector<string> packages_to_visit;
  auto* cpp_root_dir_handle = OpenDirOrDie(cpp_root_dir);
  VisitDirEntries(cpp_root_dir_handle, [&](dirent* dir_entry) {
    // Only care about non-hidden package directories.
    string entry_name = dir_entry->d_name;
    if (dir_entry->d_type == DT_DIR && !entry_name.empty() && entry_name[0] != '.') {
      packages_to_visit.emplace_back(entry_name);
    }
  });
  closedir(cpp_root_dir_handle);

  for (auto package : packages_to_visit) {
    std::cerr << "Loading package " << package << "..." << std::endl;
    LoadPackage(cpp_root_dir, package);
  }

  std::cerr << "Dumping tables... " << std::endl << std::endl;

  // Dump loaded tables.
  std::cerr << "package -> file manifest" << std::endl;
  DumpTable(package_to_file);
  std::cerr << "package -> main manifest" << std::endl;
  DumpTable(package_to_main);
  std::cerr << "file -> undef sym manifest" << std::endl;
  DumpTable(file_to_undefined_symbols);
  std::cerr << "exported sym -> file manifest" << std::endl;
  for (const auto& entry : exported_symbol_to_file) {
    std:: cerr << entry.first << " -> " << entry.second << std::endl;
  }

  // Now that we have the necessary symbol information, create and walk a
  // dependency graph of symbols starting from the target package and link shit.
  LinkDeps(target);

  std::cout << "Built package" << std::endl;
  return 0;
}
