workspace(name = "onvif")

load("//bazel:pkg_config.bzl", "pkg_config_library")
load("//bazel:arm64_sysroot.bzl", "arm64_sysroot")

# Host (x86-64) system libraries via pkg-config.
# extra_linkopts supplies transitive static deps that pkg-config --static misses.
pkg_config_library(name = "libxml2", pkg = "libxml-2.0",
    extra_linkopts = ["-licui18n", "-licuuc", "-licudata", "-llzma", "-lz"])
pkg_config_library(name = "libcurl", pkg = "libcurl",
    extra_linkopts = ["-lunistring", "-lrtmp", "-lnettle", "-lhogweed", "-lgmp",
                      "-lgnutls", "-ltasn1", "-lp11-kit",
                      "-lgcrypt", "-lgpg-error",
                      "-lsasl2", "-lbrotlicommon",
                      "-lgssapi_krb5", "-lssl", "-lcrypto",
                      "-Wl,--allow-multiple-definition"])
pkg_config_library(name = "openssl",       pkg = "openssl")
pkg_config_library(name = "sqlite3",       pkg = "sqlite3")
pkg_config_library(name = "libmicrohttpd", pkg = "libmicrohttpd",
    extra_linkopts = ["-lgnutls", "-lhogweed", "-lnettle", "-lgmp",
                      "-ltasn1", "-lunistring", "-lp11-kit"])
pkg_config_library(name = "libpq", pkg = "libpq",
    extra_linkopts = ["-lpgcommon", "-lpgport", "-lgssapi_krb5", "-lssl", "-lcrypto"])

# arm64 cross-compilation sysroot + toolchain.
# Declares the repository; packages are downloaded only when --config=arm64
# is used (via --extra_toolchains in .bazelrc), not on every host build.
arm64_sysroot(name = "arm64_sysroot")
