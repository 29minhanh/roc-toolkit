#include <CppUTest/TestHarness.h>
#include "roc_core/thread.h"
#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

uint64_t NAMELEN = 32;

namespace roc{
namespace core{

TEST_GROUP(threads) {
    
class someThread : public Thread{
    public:
    someThread(){
        start();
    }

    ~someThread(){

    }

    private:
    void run(){
        sleep(5);
    }
};

};


TEST (threads, FAIL){
    FAIL("fail me!!");
}

TEST(threads, get_thread_default_name){
someThread st;

char *expected = strdup("roc-test-core");
char *actual = strdup("");

st.get_name(actual);

CHECK(strcmp(expected, actual) == 0);

st.join();
}


TEST(threads, set_thread){
someThread st;

char *expected = strdup("roc-foo");
char *actual = strdup("");

st.set_name(expected);
st.get_name(actual);

CHECK(strcmp(expected, actual) == 0);

st.join();
}


} // namespace core
} // namespace roc
