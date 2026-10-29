# Updater dependencies

This build uses curl 8.4.0, mbedTLS 2.28.8 and Jansson 2.14 with the official
devkitPro 3DS patches. Source URLs and SHA-256 digests are in sources.json.
The bundled Mozilla CA set comes from curl.se and is installed as
the binary (platform/n3ds/update/update_ca.pem, curl.se/ca/cacert.pem, embedded with bin2s). Respective licenses are included here.

Build static ARM libraries with the devkitPro 3DS toolchain, then pass
-DUPDATE_DEPS_ROOT=/path/to/prefix to CMake (or export UPDATE_DEPS_ROOT
when using build.sh). mbedTLS enables hardware entropy and CMAC, disables
platform entropy, self-tests and timing; curl uses mbedTLS, HTTP/1.1 and
synchronous DNS, with IPv6, Unix sockets, threaded resolver, NTLM helper,
manual, pthreads, socketpair, LDAP and LDAPS disabled. Optional compression,
IDN, HTTP/2 and SSH libraries are excluded. When using devkitPro's installed
curl, CMake also links its optional zlib dependency from that SDK. Jansson is static without tests.

`build3ds.sh` builds these pinned sources automatically when the
portlibs prefix does not provide them. To prepare a separate private prefix:

```sh
python3 platform/n3ds/tools/build_update_deps.py /path/to/build/update-deps
UPDATE_DEPS_ROOT=/path/to/build/update-deps/prefix ./build3ds.sh
```

The script verifies source hashes, applies the bundled patches and keeps all
third-party source/build products outside the tracked source files.
