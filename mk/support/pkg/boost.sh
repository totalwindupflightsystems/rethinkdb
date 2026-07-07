
version=1.85.0

src_url=https://archives.boost.io/release/${version}/source/boost_${version//./_}.tar.bz2
src_url_sha256=7009fe1faa1697476bdc7027703a2badb84e849b7b0baad5086b087b971f8617

pkg_install-include () {
    mkdir -p "$install_dir/include/boost"
    cp -a "$src_dir/boost/." "$install_dir/include/boost"
}

pkg_install () {
    pkg_copy_src_to_build
    in_dir "$build_dir" ./bootstrap.sh --with-libraries=system
    in_dir "$build_dir" ./b2
    mkdir -p "$install_dir/lib"
    in_dir "$build_dir" cp "stage/lib/libboost_system.a" "$install_dir/lib"
}

pkg_install-windows () {
    : # Include files only
}
