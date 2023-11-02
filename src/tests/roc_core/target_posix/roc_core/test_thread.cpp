#include <iostream>
#include <CppUTest/TestHarness.h>
// #include <thread.h>
#include "roc_core/target_posix/roc_core/thread.h"

TEST_GROUP(threads) {};

TEST(threads, FAIL){
    FAIL("fail me!");
}