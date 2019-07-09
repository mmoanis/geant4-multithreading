cmake_minimum_required(VERSION 3.0)

# Set standard build flags
SET(COMPILER_FLAGS -pedantic -Wall -Wextra -Wcast-align -Wcast-qual -Wconversion -Wuseless-cast -Wzero-as-null-pointer-constant -Wdisabled-optimization -Wformat=2 -Winit-self -Wlogical-op -Wmissing-declarations -Wmissing-include-dirs -Wnoexcept -Wold-style-cast -Woverloaded-virtual -Wredundant-decls -Wsign-conversion -Wsign-promo -Wstrict-null-sentinel -Wstrict-overflow=5 -Wswitch-default -Wundef -Werror -Wshadow -Wformat-security -Wdeprecated -fdiagnostics-color=auto -Wheader-hygiene)

# Require a C++14 compiler but do allow extensions
SET(CMAKE_CXX_STANDARD 14)
SET(CMAKE_CXX_STANDARD_REQUIRED ON)
SET(CMAKE_CXX_EXTENSIONS OFF)

SET(CMAKE_BUILD_TYPE "RelWithDebInfo")

# Include Threads
FIND_PACKAGE(Threads REQUIRED)

# Geant4 is required for geometry description and charge deposition.
FIND_PACKAGE(Geant4)
IF(NOT Geant4_FOUND)
    MESSAGE(FATAL_ERROR "Could not find Geant4, make sure to source the Geant4 environment\n"
    "$ source YOUR_GEANT4_DIR/bin/geant4.sh")
ENDIF()

# Add Geant4 flags before our own flags
ADD_DEFINITIONS(${Geant4_DEFINITIONS})
SET(CMAKE_CXX_FLAGS "${Geant4_CXX_FLAGS} ${CMAKE_CXX_FLAGS}")
IF(CMAKE_BUILD_TYPE MATCHES DEBUG)
    SET(CMAKE_CXX_FLAGS "${Geant4_CXX_FLAGS_DEBUG} ${CMAKE_CXX_FLAGS}")
ELSEIF(CMAKE_BUILD_TYPE MATCHES RELEASE)
    SET(CMAKE_CXX_FLAGS "${Geant4_CXX_FLAGS_RELEASE} ${CMAKE_CXX_FLAGS}")
ENDIF()

# Create executables
ADD_EXECUTABLE(g4-test-nomt main_nomt.cpp)
TARGET_INCLUDE_DIRECTORIES(g4-test-nomt SYSTEM PRIVATE ${Geant4_INCLUDE_DIRS})
TARGET_LINK_LIBRARIES(g4-test-nomt ${Geant4_LIBRARIES})

ADD_EXECUTABLE(g4-test-g4mt main_g4mt.cpp)
TARGET_INCLUDE_DIRECTORIES(g4-test-g4mt SYSTEM PRIVATE ${Geant4_INCLUDE_DIRS})
TARGET_LINK_LIBRARIES(g4-test-g4mt ${Geant4_LIBRARIES} Threads::Threads)

#ADD_EXECUTABLE(g4-test-ownmt main_ownmt.cpp MyRunManager.cpp MyWorkerRunManager.cpp)
ADD_EXECUTABLE(g4-test-ownmt main_ownmt.cpp SimpleMasterRunManager.cpp SimpleWorkerRunManager.cpp Module.cpp)

TARGET_INCLUDE_DIRECTORIES(g4-test-ownmt SYSTEM PRIVATE ${Geant4_INCLUDE_DIRS})
TARGET_LINK_LIBRARIES(g4-test-ownmt ${Geant4_LIBRARIES} Threads::Threads)

ADD_EXECUTABLE(rng-test-nomt main_rng.cpp)
TARGET_INCLUDE_DIRECTORIES(rng-test-nomt SYSTEM PRIVATE ${Geant4_INCLUDE_DIRS})
TARGET_LINK_LIBRARIES(rng-test-nomt ${Geant4_LIBRARIES})
