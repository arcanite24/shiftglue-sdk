#include <catch2/catch_test_macros.hpp>

#include <rex/system/interfaces/graphics.h>

namespace {
bool Claim(const rex::system::NativeGuestOutputRenderContext&) { return true; }
bool Yield(const rex::system::NativeGuestOutputRenderContext&) { return false; }
}  // namespace

TEST_CASE("native guest output registration defaults to inert yield") {
  rex::system::NativeGuestOutputRendererRegistration registration;
  rex::system::NativeGuestOutputRenderContext context;
  CHECK_FALSE(registration.IsRegistered());
  CHECK(registration.Get() == nullptr);
  CHECK_FALSE(registration.Invoke(context));
}

TEST_CASE("native guest output registration preserves callback result") {
  rex::system::NativeGuestOutputRendererRegistration registration;
  rex::system::NativeGuestOutputRenderContext context;
  registration.Set(&Claim);
  CHECK(registration.IsRegistered());
  CHECK(registration.Invoke(context));
  registration.Set(&Yield);
  CHECK_FALSE(registration.Invoke(context));
  registration.Set(nullptr);
  CHECK_FALSE(registration.IsRegistered());
  CHECK_FALSE(registration.Invoke(context));
}
