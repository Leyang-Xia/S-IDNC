cmake_minimum_required(VERSION 3.16)
project(S_IDNC C)

set(CMAKE_C_STANDARD 11)

include_directories(.)

add_executable(S_IDNC
    main.c
    Receiver.c
    Receiver.h
    Sender.c
    Sender.h
    Symbol.c
    Symbol.h)
