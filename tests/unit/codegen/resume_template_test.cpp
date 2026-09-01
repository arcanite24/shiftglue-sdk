/**
 * @file        tests/unit/codegen/resume_template_test.cpp
 * @brief       Template coverage for resumable AOT dispatch entries
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/codegen/template_registry.h>

TEST_CASE("TemplateRegistry: indirect dispatch carries and clears the requested guest address",
          "[codegen][resume]") {
  rex::codegen::TemplateRegistry registry;
  const std::string result = registry.render("codegen/_indirect_call", R"({
    "image_base": "0x82000000",
    "image_size": "0x1000000",
    "code_base": "0x82010000",
    "code_size": "0x100000",
    "thunk_reserve_size": "0x1000"
  })");
  CHECK(result.find("ctx.dispatch_address = rex_indirect_target_") != std::string::npos);
  CHECK(result.find("ctx.dispatch_address = 0") != std::string::npos);
}

TEST_CASE("TemplateRegistry: register_cpp maps resumable aliases to their owner",
          "[codegen][resume]") {
  rex::codegen::TemplateRegistry registry;
  const std::string result = registry.render("codegen/register_cpp", R"({
    "project": "test_proj",
    "is_dll": false,
    "functions": [{
      "address": "0x82010000",
      "name": "sub_82010000",
      "below_code_base": false,
      "is_import": false,
      "resume_aliases": ["0x82010004", "0x82010010"]
    }]
  })");
  CHECK(result.find("SetFunction(0x82010000, sub_82010000)") != std::string::npos);
  CHECK(result.find("SetFunction(0x82010004, sub_82010000)") != std::string::npos);
  CHECK(result.find("SetFunction(0x82010010, sub_82010000)") != std::string::npos);
}

TEST_CASE("TemplateRegistry: init_cpp maps resumable aliases for the entrypoint image",
          "[codegen][resume]") {
  rex::codegen::TemplateRegistry registry;
  const std::string result = registry.render("codegen/init_cpp", R"({
    "project": "test_proj",
    "image_base": "0x82000000",
    "image_size": "0x1000000",
    "code_base": "0x82010000",
    "code_size": "0x100000",
    "rexcrt_heap": 0,
    "config_flags": {
      "skip_lr": false,
      "ctr_as_local": false,
      "xer_as_local": false,
      "reserved_as_local": false,
      "skip_msr": false,
      "cr_as_local": false,
      "non_argument_as_local": false,
      "non_volatile_as_local": false
    },
    "has_dll_modules": false,
    "is_dll": false,
    "functions": [{
      "address": "0x82010000",
      "name": "sub_82010000",
      "below_code_base": false,
      "is_import": false,
      "resume_aliases": ["0x82010004", "0x82010010"]
    }]
  })");
  CHECK(result.find("{ 0x82010000, sub_82010000 }") != std::string::npos);
  CHECK(result.find("{ 0x82010004, sub_82010000 }") != std::string::npos);
  CHECK(result.find("{ 0x82010010, sub_82010000 }") != std::string::npos);
}
