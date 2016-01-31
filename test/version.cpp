#include <catch.hpp>

#include "helper/io.hpp"

#include <version.hpp>

#include <errors.hpp>
#include <index.hpp>
#include <package.hpp>

using namespace std;

#define MAKE_VERSION \
  RemoteIndex ri("Remote Name"); \
  Category cat("Category Name", &ri); \
  Package pkg(Package::ScriptType, "Hello", &cat); \
  Version ver("1", &pkg);

static const char *M = "[version]";

TEST_CASE("invalid", M) {
  try {
    Version ver("hello");
    FAIL();
  }
  catch(const reapack_error &e) {
    REQUIRE(string(e.what()) == "invalid version name");
  }
}

TEST_CASE("major minor patch version", M) {
  Version ver("1.2.3");
  REQUIRE(ver.name() == "1.2.3");
  REQUIRE(ver.code() == UINT64_C(1000200030000));
}

TEST_CASE("major minor version", M) {
  Version ver("1.2");
  REQUIRE(ver.name() == "1.2");
  REQUIRE(ver.code() == UINT64_C(1000200000000));
}

TEST_CASE("major version", M) {
  Version ver("1");
  REQUIRE(ver.name() == "1");
  REQUIRE(ver.code() == UINT64_C(1000000000000));
}

TEST_CASE("version with string suffix", M) {
  Version ver("1.2pre3");
  REQUIRE(ver.name() == "1.2pre3");
  REQUIRE(ver.code() == UINT64_C(1000200030000));
}

TEST_CASE("version with 4 components", M) {
  Version ver("1.2.3.4");
  REQUIRE(ver.name() == "1.2.3.4");
  REQUIRE(ver.code() == UINT64_C(1000200030004));
  REQUIRE(ver < Version("1.2.4"));
}

TEST_CASE("decimal version", M) {
  Version ver("5.05");
  REQUIRE(ver == Version("5.5"));
  REQUIRE(ver < Version("5.50"));
}

TEST_CASE("4 digits version component", M) {
  Version ver("0.2015.12.25");
  REQUIRE(ver.name() == "0.2015.12.25");
  REQUIRE(ver.code() == UINT64_C(201500120025));
}

TEST_CASE("5 digits version component", M) {
  try {
    Version ver("12345.1");
    FAIL();
  }
  catch(const reapack_error &e) {
    REQUIRE(string(e.what()) == "version component overflow");
  }
}

TEST_CASE("version with 5 components", M) {
  try {
    Version ver("1.2.3.4.5");
    FAIL();
  }
  catch(const reapack_error &e) {
    REQUIRE(string(e.what()) == "invalid version name");
  }
}

TEST_CASE("version full name", M) {
  SECTION("no package") {
    Version ver("1.0");
    REQUIRE(ver.fullName() == "v1.0");
  }

  SECTION("with package") {
    Package pkg(Package::UnknownType, "file.name");
    Version ver("1.0", &pkg);

    REQUIRE(ver.fullName() == "file.name v1.0");
  }

  SECTION("with category") {
    Category cat("Category Name");
    Package pkg(Package::UnknownType, "file.name", &cat);
    Version ver("1.0", &pkg);

    REQUIRE(ver.fullName() == "Category Name/file.name v1.0");
  }

  SECTION("with index") {
    RemoteIndex ri("Remote Name");
    Category cat("Category Name", &ri);
    Package pkg(Package::UnknownType, "file.name", &cat);
    Version ver("1.0", &pkg);

    REQUIRE(ver.fullName() == "Remote Name/Category Name/file.name v1.0");
  }
}

TEST_CASE("add source", M) {
  MAKE_VERSION

  CHECK(ver.sources().size() == 0);

  Source *src = new Source(Source::GenericPlatform, "a", "b", &ver);
  ver.addSource(src);

  CHECK(ver.mainSource() == nullptr);
  CHECK(ver.sources().size() == 1);

  REQUIRE(src->version() == &ver);
  REQUIRE(ver.source(0) == src);
}

TEST_CASE("add owned source", M) {
  MAKE_VERSION

  Version ver2("1");
  Source *src = new Source(Source::GenericPlatform, "a", "b", &ver2);

  try {
    ver.addSource(src);
    FAIL();
  }
  catch(const reapack_error &e) {
    delete src;
    REQUIRE(string(e.what()) == "source belongs to another version");
  }
}

TEST_CASE("add main source", M) {
  MAKE_VERSION

  Source *src = new Source(Source::GenericPlatform, string(), "b", &ver);
  ver.addSource(src);

  REQUIRE(ver.mainSource() == src);
}

TEST_CASE("list files", M) {
  MAKE_VERSION

  Source *src1 = new Source(Source::GenericPlatform, "file", "url", &ver);
  ver.addSource(src1);

  Path path1;
  path1.append("Scripts");
  path1.append("Remote Name");
  path1.append("Category Name");
  path1.append("file");

  const set<Path> expected{path1};
  REQUIRE(ver.files() == expected);
}

TEST_CASE("drop sources for unknown platforms", M) {
  MAKE_VERSION
  ver.addSource(new Source(Source::UnknownPlatform, "a", "b", &ver));

  REQUIRE(ver.sources().size() == 0);
}

#ifdef __APPLE__
TEST_CASE("drop windows sources on os x", M) {
  MAKE_VERSION

  ver.addSource(new Source(Source::WindowsPlatform, "a", "b", &ver));
  ver.addSource(new Source(Source::Win32Platform, "a", "b", &ver));
  ver.addSource(new Source(Source::Win64Platform, "a", "b", &ver));

  REQUIRE(ver.sources().size() == 0);
}

#ifdef __x86_64__
TEST_CASE("drop 32-bit sources on os x 64-bit", M) {
  MAKE_VERSION
  ver.addSource(new Source(Source::Darwin32Platform, "a", "b", &ver));

  REQUIRE(ver.sources().size() == 0);
}

TEST_CASE("valid sources for os x 64-bit", M) {
  MAKE_VERSION

  ver.addSource(new Source(Source::GenericPlatform, "a", "b", &ver));
  ver.addSource(new Source(Source::DarwinPlatform, "a", "b", &ver));
  ver.addSource(new Source(Source::Darwin64Platform, "a", "b", &ver));

  REQUIRE(ver.sources().size() == 3);
}
#else
TEST_CASE("drop 64-bit sources on os x 32-bit", M) {
  MAKE_VERSION
  ver.addSource(new Source(Source::Darwin64Platform, "a", "b", &ver));

  REQUIRE(ver.sources().size() == 0);
}

TEST_CASE("valid sources for os x 32-bit", M) {
  MAKE_VERSION

  ver.addSource(new Source(Source::GenericPlatform, "a", "b", &ver));
  ver.addSource(new Source(Source::DarwinPlatform, "a", "b", &ver));
  ver.addSource(new Source(Source::Darwin32Platform, "a", "b", &ver));

  REQUIRE(ver.sources().size() == 3);
}
#endif

#elif _WIN32
TEST_CASE("drop os x sources on windows", M) {
  MAKE_VERSION

  ver.addSource(new Source(Source::DarwinPlatform, "a", "b", &ver));
  ver.addSource(new Source(Source::Darwin32Platform, "a", "b", &ver));
  ver.addSource(new Source(Source::Darwin64Platform, "a", "b", &ver));

  REQUIRE(ver.sources().size() == 0);
}

#ifdef _WIN64
TEST_CASE("drop 32-bit sources on windows 64-bit", M) {
  MAKE_VERSION
  ver.addSource(new Source(Source::Win32Platform, "a", "b", &ver));

  REQUIRE(ver.sources().size() == 0);
}

TEST_CASE("valid sources for windows 64-bit", M) {
  MAKE_VERSION

  ver.addSource(new Source(Source::GenericPlatform, "a", "b", &ver));
  ver.addSource(new Source(Source::WindowsPlatform, "a", "b", &ver));
  ver.addSource(new Source(Source::Win64Platform, "a", "b", &ver));

  REQUIRE(ver.sources().size() == 3);
}
#else
TEST_CASE("drop 64-bit sources on windows 32-bit", M) {
  MAKE_VERSION
  ver.addSource(new Source(Source::Win64Platform, "a", "b", &ver));

  REQUIRE(ver.sources().size() == 0);
}

TEST_CASE("valid sources for windows 32-bit", M) {
  MAKE_VERSION

  ver.addSource(new Source(Source::GenericPlatform, "a", "b", &ver));
  ver.addSource(new Source(Source::WindowsPlatform, "a", "b", &ver));
  ver.addSource(new Source(Source::Win32Platform, "a", "b", &ver));

  REQUIRE(ver.sources().size() == 3);
}
#endif
#endif
