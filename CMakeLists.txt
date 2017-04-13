cmake_minimum_required(VERSION 3.6)
project(shell)

set(CMAKE_CXX_STANDARD 11)

set(SOURCE_FILES main.c)
add_executable(shell ${SOURCE_FILES})
target_link_libraries(shell csapp pthread)


project(t)
set(t_file t.c)
add_executable(t ${t_file})
target_link_libraries(t csapp)

project(t2)
set(t2_file t2.c)
add_executable(t2 ${t2_file})
target_link_libraries(t2 csapp pthread)

project(sig)
set(sig_file sig.c)
add_executable(sig ${sig_file})
target_link_libraries(sig csapp pthread)