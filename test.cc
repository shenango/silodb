#include "thread.h"
#include "txn.h"
#include "btree.h"
#include "txn_btree.h"

using namespace std;
class main_thread : public ndb_thread {
public:
  main_thread(int argc, char **argv)
    : argc(argc), argv(argv), ret(0)
  {}

  virtual void
  run()
  {
    //transaction::Test();
    btree::Test();
    //txn_btree::Test();
    ret = 0;
  }

  inline int
  retval() const
  {
    return ret;
  }
private:
  int argc;
  char **argv;
  volatile int ret;
};

int
main(int argc, char **argv)
{
  main_thread t(argc, argv);
  t.start();
  t.join();
  return t.retval();
}