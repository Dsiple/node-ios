#!/bin/sh
set -x

rm -f {Debug,Release}_{torque,bytecode_builtins_list_generator}

rm -rf out

GYP_DEFINES="host_arch=x86_64 host_os=mac" CC_host="clang -x c -m64" CXX="g++ -m64 -stdlib=libc++ -std=c++17" python configure.py --dest-os=mac --without-snapshot --openssl-no-asm --without-intl --enable-static --verbose --debug
cd out || exit $?
make -j4 torque bytecode_builtins_list_generator || exit $?
cp Debug/torque ../Debug_torque || exit $?
cp Debug/bytecode_builtins_list_generator ../Debug_bytecode_builtins_list_generator || exit $?
cd .. || exit $?

rm -rf out

GYP_DEFINES="host_arch=x86_64 host_os=mac" CC_host="clang -x c -m64" CXX="g++ -m64 -stdlib=libc++ -std=c++17" python configure.py --dest-os=mac --without-snapshot --openssl-no-asm --without-intl --enable-static --verbose
cd out || exit $?
make -j4 torque bytecode_builtins_list_generator || exit $?
cp Release/torque ../Release_torque || exit $?
cp Release/bytecode_builtins_list_generator ../Release_bytecode_builtins_list_generator || exit $?
cd .. || exit $?
set +x

rm -rf out
