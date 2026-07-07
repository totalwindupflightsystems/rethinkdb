github_user=quickjs-ng
version=0.15.1

src_url=https://github.com/${github_user}/quickjs/archive/refs/tags/v${version}.tar.gz
src_url_sha256=c4e813951b7c46845096a948e978c620b11ab4cf5fd622ca09c727ec31f42623

pkg_configure () {
    # quickjs-ng uses CMake, not plain Makefile
    mkdir -p "$build_dir/build"
    in_dir "$build_dir/build" cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$(niceabspath "$install_dir")" \
        -DQJS_ENABLE_INSTALL=ON \
        -DQJS_BUILD_LIBC=OFF \
        -DBUILD_SHARED_LIBS=OFF
}

pkg_link-flags () {
    local lib="$install_dir/lib/libqjs.a"
    if [[ ! -e "$lib" ]]; then
        echo "quickjs.sh: error: static library was not built: $lib" >&2
        exit 1
    fi
    echo "$lib"
}

pkg_install-include () {
    test -e "$install_dir/include" && rm -rf "$install_dir/include"
    mkdir -p "$install_dir/include/quickjs"
    cp "$src_dir"/quickjs.h "$install_dir/include/quickjs"
    cp "$src_dir"/quickjs-libc.h "$install_dir/include/quickjs"
}

pkg_install () {
    if ! fetched; then
        error "cannot install package, it has not been fetched"
    fi
    pkg_copy_src_to_build
    pkg_configure ${configure_flags:-}
    # Build the static library via CMake
    in_dir "$build_dir/build" "$EXTERN_MAKE" -j$(nproc) qjs
    mkdir -p "$install_dir/lib"
    install -m644 "$build_dir/build/libqjs.a" "$install_dir/lib/libqjs.a"
}

pkg_install-windows () {
    if ! fetched; then
        error "cannot install package, it has not been fetched"
    fi
    pkg_copy_src_to_build

    in_dir "$build_dir" premake5 vs2017
    in_dir "$build_dir" "$MSBUILD" /nologo /p:Configuration=$CONFIGURATION /p:Platform=$PLATFORM /p:PlatformToolset=v141 /p:WindowsTargetPlatformVersion=10.0.19041.0 .build/vs2017/quickjs.vcxproj
    cp "$build_dir/.bin/$CONFIGURATION/$PLATFORM/quickjs.lib" "$windows_deps_libs"
}
