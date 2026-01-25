cmake_minimum_required(VERSION 3.10)

PROJECT(ubox C)
ADD_DEFINITIONS(-Os -ggdb -Wall -Werror --std=gnu11 -Wmissing-declarations)

SET(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")

IF(APPLE)
  INCLUDE_DIRECTORIES(/opt/local/include)
  LINK_DIRECTORIES(/opt/local/lib)
ENDIF()

find_library(json NAMES json-c json)

IF(DEBUG)
  ADD_DEFINITIONS(-DDEBUG -g3)
ENDIF()

INCLUDE (CheckSymbolExists)
CHECK_SYMBOL_EXISTS(SYS_getrandom syscall.h getrandom)

IF(getrandom)
  ADD_EXECUTABLE(getrandom getrandom.c)
  INSTALL(TARGETS getrandom
          RUNTIME DESTINATION bin
  )
ELSE()
  message( FATAL_ERROR "Kernel too old, missing SYS_getrandom system call")
ENDIF()

ADD_EXECUTABLE(kmodloader kmodloader.c)
TARGET_LINK_LIBRARIES(kmodloader ubox)

INSTALL(TARGETS kmodloader
	RUNTIME DESTINATION sbin
)

ADD_EXECUTABLE(lsbloader lsbloader.c)
TARGET_LINK_LIBRARIES(lsbloader ubox ubus)

INSTALL(TARGETS lsbloader
	RUNTIME DESTINATION sbin
)

ADD_LIBRARY(validate SHARED validate/validate.c)
INSTALL(TARGETS validate
	LIBRARY DESTINATION lib
)

ADD_EXECUTABLE(validate_data validate/cli.c)
TARGET_LINK_LIBRARIES(validate_data ubox uci validate)
FIND_PATH(uci_include_dir uci.h)
TARGET_INCLUDE_DIRECTORIES(validate_data PRIVATE ${uci_include_dir})
INSTALL(TARGETS validate_data
	RUNTIME DESTINATION sbin
)

ADD_EXECUTABLE(logd log/logd.c log/syslog.c)
TARGET_LINK_LIBRARIES(logd ubox ubus udebug)
INSTALL(TARGETS logd
	RUNTIME DESTINATION sbin
)

ADD_EXECUTABLE(logread log/logread.c log/logread_socket.c log/logread_file.c)
TARGET_LINK_LIBRARIES(logread ubox ubus ${json} blobmsg_json)
# TARGET_INCLUDE_DIRECTORIES(logread PRIVATE log)
INSTALL(TARGETS logread
	RUNTIME DESTINATION sbin
)
