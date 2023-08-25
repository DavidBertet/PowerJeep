cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(PowerJeep)
spiffs_create_partition_image(spiffs data)