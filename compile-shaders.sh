#!/bin/bash
compile() {
    glslang -V -D"$1"=main -x -o "$3" "$2"
}

src_dir=lib/source/shaders
dest_dir=lib/source/shaders/gen

compile main $src_dir/primitive.vert $dest_dir/primitive.vert.inc || exit $?
compile filterCubeMap $src_dir/filter.frag $dest_dir/filter_cube_map.frag.inc || exit $?
compile panoramaToCubeMap $src_dir/filter.frag $dest_dir/panorama_to_cube_map.frag.inc || exit $?
