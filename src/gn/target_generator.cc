// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gn/target_generator.h"

#include <stddef.h>

#include <memory>
#include <utility>

#include "gn/action_target_generator.h"
#include "gn/binary_target_generator.h"
#include "gn/build_settings.h"
#include "gn/bundle_data_target_generator.h"
#include "gn/config.h"
#include "gn/copy_target_generator.h"
#include "gn/create_bundle_target_generator.h"
#include "gn/err.h"
#include "gn/filesystem_utils.h"
#include "gn/functions.h"
#include "gn/generated_file_target_generator.h"
#include "gn/group_target_generator.h"
#include "gn/innerapis_publicinfo_generator.h"
#include "gn/metadata.h"
#include "gn/ohos_components.h"
#include "gn/ohos_variables.h"
#include "gn/parse_tree.h"
#include "gn/scheduler.h"
#include "gn/scope.h"
#include "gn/token.h"
#include "gn/value.h"
#include "gn/value_extractors.h"
#include "gn/variables.h"

TargetGenerator::TargetGenerator(Target* target,
                                 Scope* scope,
                                 const FunctionCallNode* function_call,
                                 Err* err)
    : target_(target),
      scope_(scope),
      function_call_(function_call),
      err_(err) {}

TargetGenerator::~TargetGenerator() = default;

void TargetGenerator::Run() {
  // All target types use these.

  if (!FillIncludes())
    return;

  if (!FillDependentConfigs())
    return;

  if (!FillData())
    return;

  if (!FillDependencies())
    return;

  if (!FillMetadata())
    return;

  if (!FillTestonly())
    return;

  if (!FillAssertNoDeps())
    return;

  if (!Visibility::FillItemVisibility(target_, scope_, err_))
    return;

  if (!FillWriteRuntimeDeps())
    return;

  FillCheckFlag();

  // Do type-specific generation.
  DoRun();
}

// static
void TargetGenerator::GenerateTarget(Scope* scope,
                                     const FunctionCallNode* function_call,
                                     const std::vector<Value>& args,
                                     const std::string& output_type,
                                     Err* err) {
  // Name is the argument to the function.
  if (args.size() != 1u || args[0].type() != Value::STRING) {
    *err = Err(function_call, "Target generator requires one string argument.",
               "Otherwise I'm not sure what to call this target.");
    return;
  }

  // The location of the target is the directory name with no slash at the end.
  // FIXME(brettw) validate name.
  const Label& toolchain_label = ToolchainLabelForScope(scope);
  Label label(scope->GetSourceDir(), args[0].string_value(),
              toolchain_label.dir(), toolchain_label.name());

  if (g_scheduler->verbose_logging())
    g_scheduler->Log("Defining target", label.GetUserVisibleName(true));

  std::unique_ptr<Target> target = std::make_unique<Target>(
      scope->settings(), label, scope->build_dependency_files());
  target->set_defined_from(function_call);

  // Create and call out to the proper generator.
  if (output_type == functions::kBundleData) {
    BundleDataTargetGenerator generator(target.get(), scope, function_call,
                                        err);
    generator.Run();
  } else if (output_type == functions::kCreateBundle) {
    CreateBundleTargetGenerator generator(target.get(), scope, function_call,
                                          err);
    generator.Run();
  } else if (output_type == functions::kCopy) {
    CopyTargetGenerator generator(target.get(), scope, function_call, err);
    generator.Run();
  } else if (output_type == functions::kAction) {
    ActionTargetGenerator generator(target.get(), scope, function_call,
                                    Target::ACTION, err);
    generator.Run();
  } else if (output_type == functions::kActionForEach) {
    ActionTargetGenerator generator(target.get(), scope, function_call,
                                    Target::ACTION_FOREACH, err);
    generator.Run();
  } else if (output_type == functions::kExecutable) {
    BinaryTargetGenerator generator(target.get(), scope, function_call,
                                    Target::EXECUTABLE, err);
    generator.Run();
  } else if (output_type == functions::kGroup) {
    GroupTargetGenerator generator(target.get(), scope, function_call, err);
    generator.Run();
  } else if (output_type == functions::kLoadableModule) {
    BinaryTargetGenerator generator(target.get(), scope, function_call,
                                    Target::LOADABLE_MODULE, err);
    generator.Run();
  } else if (output_type == functions::kSharedLibrary) {
    BinaryTargetGenerator generator(target.get(), scope, function_call,
                                    Target::SHARED_LIBRARY, err);
    generator.Run();
  } else if (output_type == functions::kSourceSet) {
    BinaryTargetGenerator generator(target.get(), scope, function_call,
                                    Target::SOURCE_SET, err);
    generator.Run();
  } else if (output_type == functions::kStaticLibrary) {
    BinaryTargetGenerator generator(target.get(), scope, function_call,
                                    Target::STATIC_LIBRARY, err);
    generator.Run();
  } else if (output_type == functions::kGeneratedFile) {
    GeneratedFileTargetGenerator generator(target.get(), scope, function_call,
                                           Target::GENERATED_FILE, err);
    generator.Run();
  } else if (output_type == functions::kRustLibrary) {
    BinaryTargetGenerator generator(target.get(), scope, function_call,
                                    Target::RUST_LIBRARY, err);
    generator.Run();
  } else if (output_type == functions::kRustProcMacro) {
    BinaryTargetGenerator generator(target.get(), scope, function_call,
                                    Target::RUST_PROC_MACRO, err);
    generator.Run();
  } else {
    *err = Err(function_call, "Not a known target type",
               "I am very confused by the target type \"" + output_type + "\"");
  }

  if (err->has_error())
    return;

  // Save this target for the file.
  Scope::ItemVector* collector = scope->GetItemCollector();
  if (!collector) {
    *err = Err(function_call, "Can't define a target in this context.");
    return;
  }
  collector->push_back(std::move(target));
}

const BuildSettings* TargetGenerator::GetBuildSettings() const {
  return scope_->settings()->build_settings();
}

bool TargetGenerator::FillSources() {
  const Value* value = scope_->GetValue(variables::kSources, true);
  if (!value)
    return true;

  Target::FileList dest_sources;
  if (!ExtractListOfRelativeFiles(scope_->settings()->build_settings(), *value,
                                  scope_->GetSourceDir(), &dest_sources, err_))
    return false;
  target_->sources() = std::move(dest_sources);
  return true;
}

bool TargetGenerator::FillIncludes() {
  const Value* value = scope_->GetValue(variables::kIncludeDirs, true);
  if (!value)
    return true;

  std::vector<SourceDir> dest_includes;
  if (!ExtractListOfRelativeDirs(scope_->settings()->build_settings(), *value,
                                  scope_->GetSourceDir(), &dest_includes, err_))
    return false;
  target_->include_dirs() = std::move(dest_includes);
  return true;
}

bool TargetGenerator::FillPublic() {
  const Value* value = scope_->GetValue(variables::kPublic, true);
  if (!value)
    return true;

  // If the public headers are defined, don't default to public.
  target_->set_all_headers_public(false);

  Target::FileList dest_public;
  if (!ExtractListOfRelativeFiles(scope_->settings()->build_settings(), *value,
                                  scope_->GetSourceDir(), &dest_public, err_))
    return false;
  target_->public_headers() = std::move(dest_public);
  return true;
}

bool TargetGenerator::FillOwnConfigs() {
  Scope::ItemVector *collector = scope_->GetItemCollector();
    for (const auto &config : target_->configs()) {
        std::string label = config.label.GetUserVisibleName(false);
        for (auto &item : *collector) {
            if (item->label().GetUserVisibleName(false) != label) {
                continue;
            }
            target_->own_configs().push_back(config);
        }
    }
    return true;
}

bool TargetGenerator::FillConfigs() {
  if (!FillGenericConfigs(variables::kConfigs, &target_->configs())) {
    return false;
  }
  return FillOwnConfigs();
}

bool TargetGenerator::FillDependentConfigs() {
  if (!FillGenericConfigs(variables::kAllDependentConfigs,
                          &target_->all_dependent_configs()))
    return false;

  if (!FillGenericConfigs(variables::kPublicConfigs,
                          &target_->public_configs()))
    return false;

  if (!FillGenericConfigs(variables::kAllDependentConfigs,
                          &target_->own_all_dependent_configs()))
    return false;

  if (!FillGenericConfigs(variables::kPublicConfigs,
                          &target_->own_public_configs()))
    return false;

  return true;
}

bool TargetGenerator::FillData() {
  const Value* value = scope_->GetValue(variables::kData, true);
  if (!value)
    return true;
  if (!value->VerifyTypeIs(Value::LIST, err_))
    return false;

  const std::vector<Value>& input_list = value->list_value();
  std::vector<std::string>& output_list = target_->data();
  output_list.reserve(input_list.size());

  const SourceDir& dir = scope_->GetSourceDir();
  const std::string& root_path =
      scope_->settings()->build_settings()->root_path_utf8();

  for (size_t i = 0; i < input_list.size(); i++) {
    const Value& input = input_list[i];
    if (!input.VerifyTypeIs(Value::STRING, err_))
      return false;
    const std::string input_str = input.string_value();

    // Treat each input as either a file or a directory, depending on the
    // last character.
    bool as_dir = !input_str.empty() && input_str[input_str.size() - 1] == '/';

    std::string resolved =
        dir.ResolveRelativeAs(!as_dir, input, err_, root_path, &input_str);
    if (err_->has_error())
      return false;

    output_list.push_back(resolved);
  }
  return true;
}

bool TargetGenerator::FillDependencies() {
  if (!FillGenericDepsWithWholeArchive(variables::kDeps, &target_->private_deps(),
    &target_->whole_archive_deps(), &target_->no_whole_archive_deps()))
    return false;
  if (!FillGenericDepsWithWholeArchive(variables::kPublicDeps, &target_->public_deps(),
    &target_->whole_archive_deps(), &target_->no_whole_archive_deps()))
    return false;
  if (scope_->settings()->build_settings()->is_ohos_components_enabled()) {
    if (!FillOhosComponentDeps(variables::kExternalDeps, &target_->private_deps(),
        &target_->whole_archive_deps(), &target_->no_whole_archive_deps()))
      return false;
    if (!FillOhosComponentDeps(variables::kPublicExternalDeps, &target_->public_deps(),
        &target_->whole_archive_deps(), &target_->no_whole_archive_deps()))
      return false;
  }
  if (!FillGenericDeps(variables::kDataDeps, &target_->data_deps()))
    return false;
  if (!FillGenericDeps(variables::kGenDeps, &target_->gen_deps()))
    return false;

  // "data_deps" was previously named "datadeps". For backwards-compat, read
  // the old one if no "data_deps" were specified.
  if (!scope_->GetValue(variables::kDataDeps, false)) {
    if (!FillGenericDeps("datadeps", &target_->data_deps()))
      return false;
  }

  return true;
}

bool TargetGenerator::FillMetadata() {
  // Need to get a mutable value to mark all values in the scope as used. This
  // cannot be done on a const Scope.
  Value* value = scope_->GetMutableValue(variables::kMetadata,
                                         Scope::SEARCH_CURRENT, true);

  if (!value)
    return true;

  if (!value->VerifyTypeIs(Value::SCOPE, err_))
    return false;

  Scope* scope_value = value->scope_value();

  scope_value->GetCurrentScopeValues(&target_->metadata().contents());
  scope_value->MarkAllUsed();

  // Metadata values should always hold lists of Values, such that they can be
  // collected and concatenated. Any additional specific type verification is
  // done at walk time.
  for (const auto& iter : target_->metadata().contents()) {
    if (!iter.second.VerifyTypeIs(Value::LIST, err_))
      return false;
  }

  target_->metadata().set_source_dir(scope_->GetSourceDir());
  target_->metadata().set_origin(value->origin());
  return true;
}

bool TargetGenerator::FillTestonly() {
  const Value* value = scope_->GetValue(variables::kTestonly, true);
  if (value) {
    if (!value->VerifyTypeIs(Value::BOOLEAN, err_))
      return false;
    target_->set_testonly(value->boolean_value());
  }
  return true;
}

void TargetGenerator::FillCheckFlag() {
  const Value* value = scope_->GetValue("check_flag", true);
  if (value) {
    if (!value->VerifyTypeIs(Value::BOOLEAN, err_))
      return;
    target_->set_checkflag(value->boolean_value());
  }
  return;
}

bool TargetGenerator::FillAssertNoDeps() {
  const Value* value = scope_->GetValue(variables::kAssertNoDeps, true);
  if (value) {
    return ExtractListOfLabelPatterns(scope_->settings()->build_settings(),
                                      *value, scope_->GetSourceDir(),
                                      &target_->assert_no_deps(), err_);
  }
  return true;
}

bool TargetGenerator::FillOutputs(bool allow_substitutions) {
  const Value* value = scope_->GetValue(variables::kOutputs, true);
  if (!value)
    return true;

  SubstitutionList& outputs = target_->action_values().outputs();
  if (!outputs.Parse(*value, err_))
    return false;

  if (!allow_substitutions) {
    // Verify no substitutions were actually used.
    if (!outputs.required_types().empty()) {
      *err_ =
          Err(*value, "Source expansions not allowed here.",
              "The outputs of this target used source {{expansions}} but this "
              "target type\ndoesn't support them. Just express the outputs "
              "literally.");
      return false;
    }
  }

  // Check the substitutions used are valid for this purpose.
  if (!EnsureValidSubstitutions(outputs.required_types(),
                                &IsValidSourceSubstitution, value->origin(),
                                err_))
    return false;

  // Validate that outputs are in the output dir.
  CHECK(outputs.list().size() == value->list_value().size());
  for (size_t i = 0; i < outputs.list().size(); i++) {
    if (!EnsureSubstitutionIsInOutputDir(outputs.list()[i],
                                         value->list_value()[i]))
      return false;
  }
  return true;
}

bool TargetGenerator::FillCheckIncludes() {
  const Value* value = scope_->GetValue(variables::kCheckIncludes, true);
  if (!value)
    return true;
  if (!value->VerifyTypeIs(Value::BOOLEAN, err_))
    return false;
  target_->set_check_includes(value->boolean_value());
  return true;
}

bool TargetGenerator::FillOutputExtension() {
  const Value* value = scope_->GetValue(variables::kOutputExtension, true);
  if (!value)
    return true;
  if (!value->VerifyTypeIs(Value::STRING, err_))
    return false;
  target_->set_output_extension(value->string_value());
  return true;
}

bool TargetGenerator::EnsureSubstitutionIsInOutputDir(
    const SubstitutionPattern& pattern,
    const Value& original_value) {
  if (pattern.ranges().empty()) {
    // Pattern is empty, error out (this prevents weirdness below).
    *err_ = Err(original_value, "This has an empty value in it.");
    return false;
  }

  if (pattern.ranges()[0].type == &SubstitutionLiteral) {
    // If the first thing is a literal, it must start with the output dir.
    if (!EnsureStringIsInOutputDir(GetBuildSettings()->build_dir(),
                                   pattern.ranges()[0].literal,
                                   original_value.origin(), err_))
      return false;
  } else {
    // Otherwise, the first subrange must be a pattern that expands to
    // something in the output directory.
    if (!SubstitutionIsInOutputDir(pattern.ranges()[0].type)) {
      *err_ =
          Err(original_value, "File is not inside output directory.",
              "The given file should be in the output directory. Normally you\n"
              "would specify\n\"$target_out_dir/foo\" or "
              "\"{{source_gen_dir}}/foo\".");
      return false;
    }
  }

  return true;
}

bool TargetGenerator::FillGenericConfigs(const char* var_name,
                                         UniqueVector<LabelConfigPair>* dest) {
  const Value* value = scope_->GetValue(var_name, true);
  if (value) {
    ExtractListOfUniqueLabels(scope_->settings()->build_settings(), *value,
                              scope_->GetSourceDir(),
                              ToolchainLabelForScope(scope_), dest, err_);
  }
  return !err_->has_error();
}

bool TargetGenerator::FillGenericDeps(const char* var_name,
                                      LabelTargetVector* dest) {
  const Value* value = scope_->GetValue(var_name, true);
  if (value) {
    ExtractListOfLabels(scope_->settings()->build_settings(), *value,
                        scope_->GetSourceDir(), ToolchainLabelForScope(scope_),
                        dest, err_);
  }
  return !err_->has_error();
}

bool TargetGenerator::FillGenericDepsWithWholeArchive(const char* var_name, LabelTargetVector* dest,
  LabelTargetVector* whole_dest, LabelTargetVector* no_whole_dest) {
  const Value* value = scope_->GetValue(var_name, true);
  if (value) {
    ExtractListOfLabelsMapping(target_->label().GetUserVisibleName(false), scope_->settings()->build_settings(),
                               *value, scope_->GetSourceDir(), ToolchainLabelForScope(scope_),
                               dest, whole_dest, no_whole_dest, err_);
  }
  return !err_->has_error();
}

bool TargetGenerator::FillOhosComponentDeps(const char* var_name, LabelTargetVector* dest,
  LabelTargetVector* whole_dest, LabelTargetVector* no_whole_dest)
{
  const Value* value = scope_->GetValue(var_name, true);
  if (value) {
    // Append to private deps
    ExtractListOfExternalDeps(scope_->settings()->build_settings(), *value,
                              scope_->GetSourceDir(), ToolchainLabelForScope(scope_),
                              dest, whole_dest, no_whole_dest, err_);
  }
  return !err_->has_error();
}

bool TargetGenerator::FillWriteRuntimeDeps() {
  const Value* value = scope_->GetValue(variables::kWriteRuntimeDeps, true);
  if (!value)
    return true;

  // Compute the file name and make sure it's in the output dir.
  SourceFile source_file = scope_->GetSourceDir().ResolveRelativeFile(
      *value, err_, GetBuildSettings()->root_path_utf8());
  if (err_->has_error())
    return false;
  if (!EnsureStringIsInOutputDir(GetBuildSettings()->build_dir(),
                                 source_file.value(), value->origin(), err_))
    return false;
  OutputFile output_file(GetBuildSettings(), source_file);
  target_->set_write_runtime_deps_output(output_file);

  return true;
}