#include <stdio.h>
#include <iostream>
#include <iterator>
#include <list>

#include "zypp/ZYppFactory.h"
#include "zypp/PoolQuery.h"
#include "zypp/PoolQueryUtil.tcc"
#include "zypp/RepoInfo.h"
#include "zypp/Arch.h"
#include "zypp/Pathname.h"
#include "zypp/base/Regex.h"

using std::cout;
using std::endl;
using std::string;
using namespace zypp;


bool result_cb( const sat::Solvable & solvable )
{
  zypp::PoolItem pi( zypp::ResPool::instance().find( solvable ) );
  cout << pi.resolvable() << endl;
  // name: yast2-sound 2.16.2-9 i586
  return true;
}


static void init_pool()
{
  Pathname dir(TESTS_SRC_DIR);
  dir += "/zypp/data/PoolQuery";

  ZYpp::Ptr z = getZYpp();
  ZConfig::instance().setSystemArchitecture(Arch("i586"));

  RepoInfo i1; i1.setAlias("factory");
  sat::Pool::instance().addRepoSolv(dir / "factory.solv", i1);
  RepoInfo i2; i2.setAlias("factory-nonoss");
  sat::Pool::instance().addRepoSolv(dir / "factory-nonoss.solv", i2);
  RepoInfo i3; i3.setAlias("zypp_svn");
  sat::Pool::instance().addRepoSolv(dir / "zypp_svn.solv", i3);
  RepoInfo i5; i5.setAlias("pyton");
  sat::Pool::instance().addRepoSolv(dir / "python.solv", i5);
  RepoInfo i4; i4.setAlias("@System");
  sat::Pool::instance().addRepoSolv(dir / "@System.solv", i4);
}


int main (int argc, const char ** argv)
{
  // ./poolquery regex string
  if (argc == 3)
  {
    str::regex regex(argv[1], REG_EXTENDED | REG_NOSUB | REG_NEWLINE | REG_ICASE);
    cout << (str::regex_match(argv[2], regex) ? "" : "no") << "match" << endl;
  }

  init_pool();

  PoolQuery q;
  q.addAttribute(sat::SolvAttr::name, "cjson");

  /*
  PoolQuery q;
  q.addString("weather");
  q.addAttribute(sat::SolvAttr::name, "thunder");
  q.addAttribute(sat::SolvAttr::description, "storm");
  q.addKind(ResKind::package);
  q.addRepo("factory");
*/
  std::for_each(q.begin(), q.end(), &result_cb);
//  cout << q.size() << endl;
//  cout << q << endl;
  cout << "=====" << endl;
  for_(it, q.selectableBegin(), q.selectableEnd())
    cout << *it << endl;
}
