/*---------------------------------------------------------------------\
|                          ____ _   __ __ ___                          |
|                         |__  / \ / / . \ . \                         |
|                           / / \ V /|  _/  _/                         |
|                          / /__ | | | | | |                           |
|                         /_____||_| |_| |_|                           |
|                                                                      |
\---------------------------------------------------------------------*/
/** \file zypp/target/rpm/RpmDb.cc
 *
*/
#include "librpm.h"

#include <cstdlib>
#include <cstdio>
#include <ctime>

#include <iostream>
#include <fstream>
#include <sstream>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <algorithm>

#include <boost/format.hpp>

#include "zypp/base/Logger.h"
#include "zypp/base/String.h"
#include "zypp/base/Gettext.h"

#include "zypp/Date.h"
#include "zypp/Pathname.h"
#include "zypp/PathInfo.h"
#include "zypp/PublicKey.h"

#include "zypp/target/rpm/RpmDb.h"
#include "zypp/target/rpm/RpmCallbacks.h"

#include "zypp/HistoryLog.h"
#include "zypp/target/rpm/librpmDb.h"
#include "zypp/target/rpm/RpmException.h"
#include "zypp/TmpPath.h"
#include "zypp/KeyRing.h"
#include "zypp/ZYppFactory.h"
#include "zypp/ZConfig.h"

using namespace std;
using namespace zypp::filesystem;

#define WARNINGMAILPATH		"/var/log/YaST2/"
#define FILEFORBACKUPFILES	"YaSTBackupModifiedFiles"
#define MAXRPMMESSAGELINES	10000

namespace zypp
{
namespace target
{
namespace rpm
{
namespace
{
#if 1 // No more need to escape whitespace since rpm-4.4.2.3
const char* quoteInFilename_m = "\'\"";
#else
const char* quoteInFilename_m = " \t\'\"";
#endif
inline string rpmQuoteFilename( const Pathname & path_r )
{
  string path( path_r.asString() );
  for ( string::size_type pos = path.find_first_of( quoteInFilename_m );
        pos != string::npos;
        pos = path.find_first_of( quoteInFilename_m, pos ) )
  {
    path.insert( pos, "\\" );
    pos += 2; // skip '\\' and the quoted char.
  }
  return path;
}
}

struct KeyRingSignalReceiver : callback::ReceiveReport<KeyRingSignals>
{
  KeyRingSignalReceiver(RpmDb &rpmdb) : _rpmdb(rpmdb)
  {
    connect();
  }

  ~KeyRingSignalReceiver()
  {
    disconnect();
  }

  virtual void trustedKeyAdded( const PublicKey &key )
  {
    MIL << "trusted key added to zypp Keyring. Importing" << endl;
    // now import the key in rpm
    try
    {
      _rpmdb.importPubkey( key );
    }
    catch (RpmException &e)
    {
      ERR << "Could not import key " << key.id() << " (" << key.name() << " from " << key.path() << " in rpm database" << endl;
    }
  }

  virtual void trustedKeyRemoved( const PublicKey &key  )
  {
    MIL << "Trusted key removed from zypp Keyring. Removing..." << endl;

    // remove the key from rpm
    try
    {
      _rpmdb.removePubkey( key );
    }
    catch (RpmException &e)
    {
      ERR << "Could not remove key " << key.id() << " (" << key.name() << ") from rpm database" << endl;
    }
  }

  RpmDb &_rpmdb;
};

static shared_ptr<KeyRingSignalReceiver> sKeyRingReceiver;

unsigned diffFiles(const string file1, const string file2, string& out, int maxlines)
{
  const char* argv[] =
    {
      "diff",
      "-u",
      file1.c_str(),
      file2.c_str(),
      NULL
    };
  ExternalProgram prog(argv,ExternalProgram::Discard_Stderr, false, -1, true);

  //if(!prog)
  //return 2;

  string line;
  int count = 0;
  for (line = prog.receiveLine(), count=0;
       !line.empty();
       line = prog.receiveLine(), count++ )
  {
    if (maxlines<0?true:count<maxlines)
      out+=line;
  }

  return prog.close();
}



/******************************************************************
 **
 **
 **	FUNCTION NAME : stringPath
 **	FUNCTION TYPE : inline string
*/
inline string stringPath( const Pathname & root_r, const Pathname & sub_r )
{
  return librpmDb::stringPath( root_r, sub_r );
}

/******************************************************************
 **
 **
 **	FUNCTION NAME : operator<<
 **	FUNCTION TYPE : ostream &
*/
ostream & operator<<( ostream & str, const RpmDb::DbStateInfoBits & obj )
{
  if ( obj == RpmDb::DbSI_NO_INIT )
  {
    str << "NO_INIT";
  }
  else
  {
#define ENUM_OUT(B,C) str << ( obj & RpmDb::B ? C : '-' )
    str << "V4(";
    ENUM_OUT( DbSI_HAVE_V4,	'X' );
    ENUM_OUT( DbSI_MADE_V4,	'c' );
    ENUM_OUT( DbSI_MODIFIED_V4,	'm' );
    str << ")V3(";
    ENUM_OUT( DbSI_HAVE_V3,	'X' );
    ENUM_OUT( DbSI_HAVE_V3TOV4,	'B' );
    ENUM_OUT( DbSI_MADE_V3TOV4,	'c' );
    str << ")";
#undef ENUM_OUT
  }
  return str;
}



///////////////////////////////////////////////////////////////////
//
//	CLASS NAME : RpmDb
//
///////////////////////////////////////////////////////////////////

#define FAILIFNOTINITIALIZED if( ! initialized() ) { ZYPP_THROW(RpmDbNotOpenException()); }

///////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::RpmDb
//	METHOD TYPE : Constructor
//
RpmDb::RpmDb()
    : _dbStateInfo( DbSI_NO_INIT )
#warning Check for obsolete memebers
    , _backuppath ("/var/adm/backup")
    , _packagebackups(false)
    , _warndirexists(false)
{
  process = 0;
  exit_code = -1;
  librpmDb::globalInit();
  // Some rpm versions are patched not to abort installation if
  // symlink creation failed.
  setenv( "RPM_IgnoreFailedSymlinks", "1", 1 );
  sKeyRingReceiver.reset(new KeyRingSignalReceiver(*this));
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::~RpmDb
//	METHOD TYPE : Destructor
//
RpmDb::~RpmDb()
{
  MIL << "~RpmDb()" << endl;
  closeDatabase();
  delete process;
  MIL  << "~RpmDb() end" << endl;
  sKeyRingReceiver.reset();
}

Date RpmDb::timestamp() const
{
  Date ts_rpm;

  Pathname db_path;
  if ( dbPath().empty() )
    db_path = "/var/lib/rpm";
  else
    db_path = dbPath();

  PathInfo rpmdb_info(root() + db_path + "/Packages");

  if ( rpmdb_info.isExist() )
    return rpmdb_info.mtime();
  else
    return Date::now();
}
///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::dumpOn
//	METHOD TYPE : ostream &
//
ostream & RpmDb::dumpOn( ostream & str ) const
{
  str << "RpmDb[";

  if ( _dbStateInfo == DbSI_NO_INIT )
  {
    str << "NO_INIT";
  }
  else
  {
#define ENUM_OUT(B,C) str << ( _dbStateInfo & B ? C : '-' )
    str << "V4(";
    ENUM_OUT( DbSI_HAVE_V4,	'X' );
    ENUM_OUT( DbSI_MADE_V4,	'c' );
    ENUM_OUT( DbSI_MODIFIED_V4,	'm' );
    str << ")V3(";
    ENUM_OUT( DbSI_HAVE_V3,	'X' );
    ENUM_OUT( DbSI_HAVE_V3TOV4,	'B' );
    ENUM_OUT( DbSI_MADE_V3TOV4,	'c' );
    str << "): " << stringPath( _root, _dbPath );
#undef ENUM_OUT
  }
  return str << "]";
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::initDatabase
//	METHOD TYPE : PMError
//
void RpmDb::initDatabase( Pathname root_r, Pathname dbPath_r, bool doRebuild_r )
{
  ///////////////////////////////////////////////////////////////////
  // Check arguments
  ///////////////////////////////////////////////////////////////////
  bool quickinit( root_r.empty() );

  if ( root_r.empty() )
    root_r = "/";

  if ( dbPath_r.empty() )
    dbPath_r = "/var/lib/rpm";

  if ( ! (root_r.absolute() && dbPath_r.absolute()) )
  {
    ERR << "Illegal root or dbPath: " << stringPath( root_r, dbPath_r ) << endl;
    ZYPP_THROW(RpmInvalidRootException(root_r, dbPath_r));
  }

  MIL << "Calling initDatabase: " << stringPath( root_r, dbPath_r )
      << ( doRebuild_r ? " (rebuilddb)" : "" )
      << ( quickinit ? " (quickinit)" : "" ) << endl;

  ///////////////////////////////////////////////////////////////////
  // Check whether already initialized
  ///////////////////////////////////////////////////////////////////
  if ( initialized() )
  {
    if ( root_r == _root && dbPath_r == _dbPath )
    {
      return;
    }
    else
    {
      ZYPP_THROW(RpmDbAlreadyOpenException(_root, _dbPath, root_r, dbPath_r));
    }
  }

  ///////////////////////////////////////////////////////////////////
  // init database
  ///////////////////////////////////////////////////////////////////
  librpmDb::unblockAccess();

  if ( quickinit )
  {
    MIL << "QUICK initDatabase (no systemRoot set)" << endl;
    return;
  }

  DbStateInfoBits info = DbSI_NO_INIT;
  try
  {
    internal_initDatabase( root_r, dbPath_r, info );
  }
  catch (const RpmException & excpt_r)
  {
    ZYPP_CAUGHT(excpt_r);
    librpmDb::blockAccess();
    ERR << "Cleanup on error: state " << info << endl;

    if ( dbsi_has( info, DbSI_MADE_V4 ) )
    {
      // remove the newly created rpm4 database and
      // any backup created on conversion.
      removeV4( root_r + dbPath_r, dbsi_has( info, DbSI_MADE_V3TOV4 ) );
    }
    ZYPP_RETHROW(excpt_r);
  }
  if ( dbsi_has( info, DbSI_HAVE_V3 ) )
  {
    if ( root_r == "/" || dbsi_has( info, DbSI_MODIFIED_V4 ) )
    {
      // Move obsolete rpm3 database beside.
      MIL << "Cleanup: state " << info << endl;
      removeV3( root_r + dbPath_r, dbsi_has( info, DbSI_MADE_V3TOV4 ) );
      dbsi_clr( info, DbSI_HAVE_V3 );
    }
    else
    {
      // Performing an update: Keep the original rpm3 database
      // and wait if the rpm4 database gets modified by installing
      // or removing packages. Cleanup in modifyDatabase or closeDatabase.
      MIL << "Update mode: Cleanup delayed until closeOldDatabase." << endl;
    }
  }
#warning CHECK: notify root about conversion backup.

  _root   = root_r;
  _dbPath = dbPath_r;
  _dbStateInfo = info;

  if ( doRebuild_r )
  {
    if (      dbsi_has( info, DbSI_HAVE_V4 )
         && ! dbsi_has( info, DbSI_MADE_V4 ) )
    {
      rebuildDatabase();
    }
  }

  MIL << "Syncronizing keys with zypp keyring" << endl;
  // we do this one by one now.
  importZyppKeyRingTrustedKeys();
  exportTrustedKeysInZyppKeyRing();

  // Close the database in case any write acces (create/convert)
  // happened during init. This should drop any lock acquired
  // by librpm. On demand it will be reopened readonly and should
  // not hold any lock.
  librpmDb::dbRelease( true );

  MIL << "InitDatabase: " << *this << endl;
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::internal_initDatabase
//	METHOD TYPE : PMError
//
void RpmDb::internal_initDatabase( const Pathname & root_r, const Pathname & dbPath_r,
                                   DbStateInfoBits & info_r )
{
  info_r = DbSI_NO_INIT;

  ///////////////////////////////////////////////////////////////////
  // Get info about the desired database dir
  ///////////////////////////////////////////////////////////////////
  librpmDb::DbDirInfo dbInfo( root_r, dbPath_r );

  if ( dbInfo.illegalArgs() )
  {
    // should not happen (checked in initDatabase)
    ZYPP_THROW(RpmInvalidRootException(root_r, dbPath_r));
  }
  if ( ! dbInfo.usableArgs() )
  {
    ERR << "Bad database directory: " << dbInfo.dbDir() << endl;
    ZYPP_THROW(RpmInvalidRootException(root_r, dbPath_r));
  }

  if ( dbInfo.hasDbV4() )
  {
    dbsi_set( info_r, DbSI_HAVE_V4 );
    MIL << "Found rpm4 database in " << dbInfo.dbDir() << endl;
  }
  else
  {
    MIL << "Creating new rpm4 database in " << dbInfo.dbDir() << endl;
  }

  if ( dbInfo.hasDbV3() )
  {
    dbsi_set( info_r, DbSI_HAVE_V3 );
  }
  if ( dbInfo.hasDbV3ToV4() )
  {
    dbsi_set( info_r, DbSI_HAVE_V3TOV4 );
  }

  DBG << "Initial state: " << info_r << ": " << stringPath( root_r, dbPath_r );
  librpmDb::dumpState( DBG ) << endl;

  ///////////////////////////////////////////////////////////////////
  // Access database, create if needed
  ///////////////////////////////////////////////////////////////////

  // creates dbdir and empty rpm4 database if not present
  librpmDb::dbAccess( root_r, dbPath_r );

  if ( ! dbInfo.hasDbV4() )
  {
    dbInfo.restat();
    if ( dbInfo.hasDbV4() )
    {
      dbsi_set( info_r, DbSI_HAVE_V4 | DbSI_MADE_V4 );
    }
  }

  DBG << "Access state: " << info_r << ": " << stringPath( root_r, dbPath_r );
  librpmDb::dumpState( DBG ) << endl;

  ///////////////////////////////////////////////////////////////////
  // Check whether to convert something. Create backup but do
  // not remove anything here
  ///////////////////////////////////////////////////////////////////
  librpmDb::constPtr dbptr;
  librpmDb::dbAccess( dbptr );
  bool dbEmpty = dbptr->empty();
  if ( dbEmpty )
  {
    MIL << "Empty rpm4 database "  << dbInfo.dbV4() << endl;
  }

  if ( dbInfo.hasDbV3() )
  {
    MIL << "Found rpm3 database " << dbInfo.dbV3() << endl;

    if ( dbEmpty )
    {
      extern void convertV3toV4( const Pathname & v3db_r, const librpmDb::constPtr & v4db_r );
      convertV3toV4( dbInfo.dbV3().path(), dbptr );

      // create a backup copy
      int res = filesystem::copy( dbInfo.dbV3().path(), dbInfo.dbV3ToV4().path() );
      if ( res )
      {
        WAR << "Backup converted rpm3 database failed: error(" << res << ")" << endl;
      }
      else
      {
        dbInfo.restat();
        if ( dbInfo.hasDbV3ToV4() )
        {
          MIL << "Backup converted rpm3 database: " << dbInfo.dbV3ToV4() << endl;
          dbsi_set( info_r, DbSI_HAVE_V3TOV4 | DbSI_MADE_V3TOV4 );
        }
      }

    }
    else
    {

      WAR << "Non empty rpm3 and rpm4 database found: using rpm4" << endl;
      // set DbSI_MODIFIED_V4 as it's not a temporary which can be removed.
      dbsi_set( info_r, DbSI_MODIFIED_V4 );

    }

    DBG << "Convert state: " << info_r << ": " << stringPath( root_r, dbPath_r );
    librpmDb::dumpState( DBG ) << endl;
  }

  if ( dbInfo.hasDbV3ToV4() )
  {
    MIL << "Rpm3 database backup: " << dbInfo.dbV3ToV4() << endl;
  }
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::removeV4
//	METHOD TYPE : void
//
void RpmDb::removeV4( const Pathname & dbdir_r, bool v3backup_r )
{
  const char * v3backup = "packages.rpm3";
  const char * master = "Packages";
  const char * index[] =
    {
      "Basenames",
      "Conflictname",
      "Depends",
      "Dirnames",
      "Filemd5s",
      "Group",
      "Installtid",
      "Name",
      "Providename",
      "Provideversion",
      "Pubkeys",
      "Requirename",
      "Requireversion",
      "Sha1header",
      "Sigmd5",
      "Triggername",
      // last entry!
      NULL
    };

  PathInfo pi( dbdir_r );
  if ( ! pi.isDir() )
  {
    ERR << "Can't remove rpm4 database in non directory: " << dbdir_r << endl;
    return;
  }

  for ( const char ** f = index; *f; ++f )
  {
    pi( dbdir_r + *f );
    if ( pi.isFile() )
    {
      filesystem::unlink( pi.path() );
    }
  }

  pi( dbdir_r + master );
  if ( pi.isFile() )
  {
    MIL << "Removing rpm4 database " << pi << endl;
    filesystem::unlink( pi.path() );
  }

  if ( v3backup_r )
  {
    pi( dbdir_r + v3backup );
    if ( pi.isFile() )
    {
      MIL << "Removing converted rpm3 database backup " << pi << endl;
      filesystem::unlink( pi.path() );
    }
  }
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::removeV3
//	METHOD TYPE : void
//
void RpmDb::removeV3( const Pathname & dbdir_r, bool v3backup_r )
{
  const char * master = "packages.rpm";
  const char * index[] =
    {
      "conflictsindex.rpm",
      "fileindex.rpm",
      "groupindex.rpm",
      "nameindex.rpm",
      "providesindex.rpm",
      "requiredby.rpm",
      "triggerindex.rpm",
      // last entry!
      NULL
    };

  PathInfo pi( dbdir_r );
  if ( ! pi.isDir() )
  {
    ERR << "Can't remove rpm3 database in non directory: " << dbdir_r << endl;
    return;
  }

  for ( const char ** f = index; *f; ++f )
  {
    pi( dbdir_r + *f );
    if ( pi.isFile() )
    {
      filesystem::unlink( pi.path() );
    }
  }

#warning CHECK: compare vs existing v3 backup. notify root
  pi( dbdir_r + master );
  if ( pi.isFile() )
  {
    Pathname m( pi.path() );
    if ( v3backup_r )
    {
      // backup was already created
      filesystem::unlink( m );
      Pathname b( m.extend( "3" ) );
      pi( b ); // stat backup
    }
    else
    {
      Pathname b( m.extend( ".deleted" ) );
      pi( b );
      if ( pi.isFile() )
      {
        // rempve existing backup
        filesystem::unlink( b );
      }
      filesystem::rename( m, b );
      pi( b ); // stat backup
    }
    MIL << "(Re)moved rpm3 database to " << pi << endl;
  }
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::modifyDatabase
//	METHOD TYPE : void
//
void RpmDb::modifyDatabase()
{
  if ( ! initialized() )
    return;

  // tag database as modified
  dbsi_set( _dbStateInfo, DbSI_MODIFIED_V4 );

  // Move outdated rpm3 database beside.
  if ( dbsi_has( _dbStateInfo, DbSI_HAVE_V3 ) )
  {
    MIL << "Update mode: Delayed cleanup: state " << _dbStateInfo << endl;
    removeV3( _root + _dbPath, dbsi_has( _dbStateInfo, DbSI_MADE_V3TOV4 ) );
    dbsi_clr( _dbStateInfo, DbSI_HAVE_V3 );
  }
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::closeDatabase
//	METHOD TYPE : PMError
//
void RpmDb::closeDatabase()
{
  if ( ! initialized() )
  {
    return;
  }

  MIL << "Calling closeDatabase: " << *this << endl;

  ///////////////////////////////////////////////////////////////////
  // Block further database access
  ///////////////////////////////////////////////////////////////////
  librpmDb::blockAccess();

  ///////////////////////////////////////////////////////////////////
  // Check fate if old version database still present
  ///////////////////////////////////////////////////////////////////
  if ( dbsi_has( _dbStateInfo, DbSI_HAVE_V3 ) )
  {
    MIL << "Update mode: Delayed cleanup: state " << _dbStateInfo << endl;
    if ( dbsi_has( _dbStateInfo, DbSI_MODIFIED_V4 ) )
    {
      // Move outdated rpm3 database beside.
      removeV3( _root + _dbPath, dbsi_has( _dbStateInfo, DbSI_MADE_V3TOV4 )  );
    }
    else
    {
      // Remove unmodified rpm4 database
      removeV4( _root + _dbPath, dbsi_has( _dbStateInfo, DbSI_MADE_V3TOV4 ) );
    }
  }

  ///////////////////////////////////////////////////////////////////
  // Uninit
  ///////////////////////////////////////////////////////////////////
  _root = _dbPath = Pathname();
  _dbStateInfo = DbSI_NO_INIT;

  MIL << "closeDatabase: " << *this << endl;
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::rebuildDatabase
//	METHOD TYPE : PMError
//
void RpmDb::rebuildDatabase()
{
  callback::SendReport<RebuildDBReport> report;

  report->start( root() + dbPath() );

  try
  {
    doRebuildDatabase(report);
  }
  catch (RpmException & excpt_r)
  {
    report->finish(root() + dbPath(), RebuildDBReport::FAILED, excpt_r.asUserHistory());
    ZYPP_RETHROW(excpt_r);
  }
  report->finish(root() + dbPath(), RebuildDBReport::NO_ERROR, "");
}

void RpmDb::doRebuildDatabase(callback::SendReport<RebuildDBReport> & report)
{
  FAILIFNOTINITIALIZED;

  MIL << "RpmDb::rebuildDatabase" << *this << endl;
  // FIXME  Timecount _t( "RpmDb::rebuildDatabase" );

  PathInfo dbMaster( root() + dbPath() + "Packages" );
  PathInfo dbMasterBackup( dbMaster.path().extend( ".y2backup" ) );

  // run rpm
  RpmArgVec opts;
  opts.push_back("--rebuilddb");
  opts.push_back("-vv");

  // don't call modifyDatabase because it would remove the old
  // rpm3 database, if the current database is a temporary one.
  run_rpm (opts, ExternalProgram::Stderr_To_Stdout);

  // progress report: watch this file growing
  PathInfo newMaster( root()
                      + dbPath().extend( str::form( "rebuilddb.%d",
                                                    process?process->getpid():0) )
                      + "Packages" );

  string       line;
  string       errmsg;

  while ( systemReadLine( line ) )
  {
    if ( newMaster() )
    { // file is removed at the end of rebuild.
      // current size should be upper limit for new db
      if ( ! report->progress( (100 * newMaster.size()) / dbMaster.size(), root() + dbPath()) )
      {
        WAR << "User requested abort." << endl;
        systemKill();
        filesystem::recursive_rmdir( newMaster.path().dirname() );
      }
    }

    if ( line.compare( 0, 2, "D:" ) )
    {
      errmsg += line + '\n';
      //      report.notify( line );
      WAR << line << endl;
    }
  }

  int rpm_status = systemStatus();

  if ( rpm_status != 0 )
  {
    //TranslatorExplanation after semicolon is error message
    ZYPP_THROW(RpmSubprocessException(string(_("RPM failed: ") +
               (errmsg.empty() ? error_message: errmsg))));
  }
  else
  {
    report->progress( 100, root() + dbPath() ); // 100%
  }
}

void RpmDb::importZyppKeyRingTrustedKeys()
{
  MIL << "Importing zypp trusted keyring" << std::endl;

  std::list<PublicKey> zypp_keys;
  zypp_keys = getZYpp()->keyRing()->trustedPublicKeys();
  /* The pubkeys() call below is expensive.  It calls gpg2 for each
     gpg-pubkey in the rpm db.  Useless if we don't have any keys in
     zypp yet.  */
  if (zypp_keys.empty())
    return;

  std::list<PublicKey> rpm_keys = pubkeys();
  for_( it, zypp_keys.begin(), zypp_keys.end() )
    {
      // we find only the left part of the long gpg key, as rpm does not support long ids
      std::list<PublicKey>::iterator ik = find( rpm_keys.begin(), rpm_keys.end(), (*it));
      if ( ik != rpm_keys.end() )
        {
          MIL << "Key " << (*it).id() << " (" << (*it).name() << ") is already in rpm database." << std::endl;
        }
      else
        {
          // now import the key in rpm
          try
            {
              importPubkey( *it );
              MIL << "Trusted key " << (*it).id() << " (" << (*it).name() << ") imported in rpm database." << std::endl;
            }
          catch (RpmException &e)
            {
              ERR << "Could not import key " << (*it).id() << " (" << (*it).name() << " from " << (*it).path() << " in rpm database" << std::endl;
            }
        }
    }
}

void RpmDb::exportTrustedKeysInZyppKeyRing()
{
  MIL << "Exporting rpm keyring into zypp trusted keyring" <<endl;
  librpmDb::db_const_iterator keepDbOpen; // just to keep a ref.

  set<Edition>    rpm_keys( pubkeyEditions() );
  list<PublicKey> zypp_keys( getZYpp()->keyRing()->trustedPublicKeys() );

  // Temporarily disconnect to prevent the attemt to re-import the exported keys.
  callback::TempConnect<KeyRingSignals> tempDisconnect;

  TmpFile tmpfile( getZYpp()->tmpPath() );
  {
    ofstream tmpos( tmpfile.path().c_str() );
    for_( it, rpm_keys.begin(), rpm_keys.end() )
    {
      // search the zypp key into the rpm keys
      // long id is edition version + release
      string id = str::toUpper( (*it).version() + (*it).release());
      list<PublicKey>::iterator ik( find( zypp_keys.begin(), zypp_keys.end(), id) );
      if ( ik != zypp_keys.end() )
      {
	MIL << "Key " << (*it) << " is already in zypp database." << endl;
      }
      else
      {
	// we export the rpm key into a file
	RpmHeader::constPtr result( new RpmHeader() );
	getData( string("gpg-pubkey"), *it, result );
	MIL <<  "Will export trusted key " << (*it) << " to zypp keyring." << endl;
	tmpos << result->tag_description() << endl;
      }
    }
  }
  try
  {
    getZYpp()->keyRing()->multiKeyImport( tmpfile.path(), true /*trusted*/);
  }
  catch (Exception &e)
  {
    ERR << "Could not import keys into in zypp keyring" << endl;
  }
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::importPubkey
//	METHOD TYPE : PMError
//
void RpmDb::importPubkey( const PublicKey & pubkey_r )
{
  FAILIFNOTINITIALIZED;

  // check if the key is already in the rpm database and just
  // return if it does.
  set<Edition> rpm_keys = pubkeyEditions();
  string keyshortid = pubkey_r.id().substr(8,8);
  MIL << "Comparing '" << keyshortid << "' to: ";
  for ( set<Edition>::const_iterator it = rpm_keys.begin(); it != rpm_keys.end(); ++it)
  {
    string id = str::toUpper( (*it).version() );
    MIL <<  ", '" << id << "'";
    if ( id == keyshortid )
    {
        // they match id
        // now check if timestamp is different
        Date date = Date(str::strtonum<Date::ValueType>("0x" + (*it).release()));
        if (  date == pubkey_r.created() )
        {

            MIL << endl << "Key " << pubkey_r << " is already in the rpm trusted keyring." << endl;
            return;
        }
        else
        {
            MIL << endl << "Key " << pubkey_r << " has another version in keyring. ( " << date << " & " << pubkey_r.created() << ")" << endl;

        }

    }
  }
  // key does not exists, lets import it
  MIL <<  endl;

  RpmArgVec opts;
  opts.push_back ( "--import" );
  opts.push_back ( "--" );
  opts.push_back ( pubkey_r.path().asString().c_str() );

  // don't call modifyDatabase because it would remove the old
  // rpm3 database, if the current database is a temporary one.
  run_rpm( opts, ExternalProgram::Stderr_To_Stdout );

  string line;
  while ( systemReadLine( line ) )
  {
    if ( line.substr( 0, 6 ) == "error:" )
    {
      WAR << line << endl;
    }
    else
    {
      DBG << line << endl;
    }
  }

  int rpm_status = systemStatus();

  if ( rpm_status != 0 )
  {
    //TranslatorExplanation first %s is file name, second is error message
    ZYPP_THROW(RpmSubprocessException(boost::str(boost::format(
        _("Failed to import public key from file %s: %s"))
        % pubkey_r.asString() % error_message)));
  }
  else
  {
    MIL << "Key " << pubkey_r << " imported in rpm trusted keyring." << endl;
  }
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::removePubkey
//	METHOD TYPE : PMError
//
void RpmDb::removePubkey( const PublicKey & pubkey_r )
{
  FAILIFNOTINITIALIZED;

  // check if the key is in the rpm database and just
  // return if it does not.
  set<Edition> rpm_keys = pubkeyEditions();

  // search the key
  set<Edition>::const_iterator found_edition = rpm_keys.end();

  for ( set<Edition>::const_iterator it = rpm_keys.begin(); it != rpm_keys.end(); ++it)
  {
    string id = str::toUpper( (*it).version() );
    string keyshortid = pubkey_r.id().substr(8,8);
    MIL << "Comparing '" << id << "' to '" << keyshortid << "'" << endl;
    if ( id == keyshortid )
    {
	found_edition = it;
	break;
    }
  }

  // the key does not exist, cannot be removed
  if (found_edition == rpm_keys.end())
  {
      WAR << "Key " << pubkey_r.id() << " is not in rpm db" << endl;
      return;
  }

  string rpm_name("gpg-pubkey-" + found_edition->asString());

  RpmArgVec opts;
  opts.push_back ( "-e" );
  opts.push_back ( "--" );
  opts.push_back ( rpm_name.c_str() );

  // don't call modifyDatabase because it would remove the old
  // rpm3 database, if the current database is a temporary one.
  run_rpm( opts, ExternalProgram::Stderr_To_Stdout );

  string line;
  while ( systemReadLine( line ) )
  {
    if ( line.substr( 0, 6 ) == "error:" )
    {
      WAR << line << endl;
    }
    else
    {
      DBG << line << endl;
    }
  }

  int rpm_status = systemStatus();

  if ( rpm_status != 0 )
  {
    //TranslatorExplanation first %s is key name, second is error message
    ZYPP_THROW(RpmSubprocessException(boost::str(boost::format(
        _("Failed to remove public key %s: %s")) % pubkey_r.asString()
        % error_message)));
  }
  else
  {
    MIL << "Key " << pubkey_r << " has been removed from RPM trusted keyring" << endl;
  }
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::pubkeys
//	METHOD TYPE : set<Edition>
//
list<PublicKey> RpmDb::pubkeys() const
{
  list<PublicKey> ret;

  librpmDb::db_const_iterator it;
  for ( it.findByName( string( "gpg-pubkey" ) ); *it; ++it )
  {
    Edition edition = it->tag_edition();
    if (edition != Edition::noedition)
    {
      // we export the rpm key into a file
      RpmHeader::constPtr result = new RpmHeader();
      getData( string("gpg-pubkey"), edition, result );
      TmpFile file(getZYpp()->tmpPath());
      ofstream os;
      try
      {
        os.open(file.path().asString().c_str());
        // dump rpm key into the tmp file
        os << result->tag_description();
        //MIL << "-----------------------------------------------" << endl;
        //MIL << result->tag_description() <<endl;
        //MIL << "-----------------------------------------------" << endl;
        os.close();
        // read the public key from the dumped file
        PublicKey key(file);
        ret.push_back(key);
      }
      catch (exception &e)
      {
        ERR << "Could not dump key " << edition.asString() << " in tmp file " << file.path() << endl;
        // just ignore the key
      }
    }
  }
  return ret;
}

set<Edition> RpmDb::pubkeyEditions() const
  {
    set<Edition> ret;

    librpmDb::db_const_iterator it;
    for ( it.findByName( string( "gpg-pubkey" ) ); *it; ++it )
    {
      Edition edition = it->tag_edition();
      if (edition != Edition::noedition)
        ret.insert( edition );
    }
    return ret;
  }


///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::fileList
//	METHOD TYPE : bool
//
//	DESCRIPTION :
//
list<FileInfo>
RpmDb::fileList( const string & name_r, const Edition & edition_r ) const
{
  list<FileInfo> result;

  librpmDb::db_const_iterator it;
  bool found;
  if (edition_r == Edition::noedition)
  {
    found = it.findPackage( name_r );
  }
  else
  {
    found = it.findPackage( name_r, edition_r );
  }
  if (!found)
    return result;

  return result;
}


///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::hasFile
//	METHOD TYPE : bool
//
//	DESCRIPTION :
//
bool RpmDb::hasFile( const string & file_r, const string & name_r ) const
{
  librpmDb::db_const_iterator it;
  bool res;
  do
  {
    res = it.findByFile( file_r );
    if (!res) break;
    if (!name_r.empty())
    {
      res = (it->tag_name() == name_r);
    }
    ++it;
  }
  while (res && *it);
  return res;
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::whoOwnsFile
//	METHOD TYPE : string
//
//	DESCRIPTION :
//
string RpmDb::whoOwnsFile( const string & file_r) const
{
  librpmDb::db_const_iterator it;
  if (it.findByFile( file_r ))
  {
    return it->tag_name();
  }
  return "";
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::hasProvides
//	METHOD TYPE : bool
//
//	DESCRIPTION :
//
bool RpmDb::hasProvides( const string & tag_r ) const
{
  librpmDb::db_const_iterator it;
  return it.findByProvides( tag_r );
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::hasRequiredBy
//	METHOD TYPE : bool
//
//	DESCRIPTION :
//
bool RpmDb::hasRequiredBy( const string & tag_r ) const
{
  librpmDb::db_const_iterator it;
  return it.findByRequiredBy( tag_r );
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::hasConflicts
//	METHOD TYPE : bool
//
//	DESCRIPTION :
//
bool RpmDb::hasConflicts( const string & tag_r ) const
{
  librpmDb::db_const_iterator it;
  return it.findByConflicts( tag_r );
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::hasPackage
//	METHOD TYPE : bool
//
//	DESCRIPTION :
//
bool RpmDb::hasPackage( const string & name_r ) const
{
  librpmDb::db_const_iterator it;
  return it.findPackage( name_r );
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::hasPackage
//	METHOD TYPE : bool
//
//	DESCRIPTION :
//
bool RpmDb::hasPackage( const string & name_r, const Edition & ed_r ) const
{
  librpmDb::db_const_iterator it;
  return it.findPackage( name_r, ed_r );
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::getData
//	METHOD TYPE : PMError
//
//	DESCRIPTION :
//
void RpmDb::getData( const string & name_r,
                     RpmHeader::constPtr & result_r ) const
{
  librpmDb::db_const_iterator it;
  it.findPackage( name_r );
  result_r = *it;
  if (it.dbError())
    ZYPP_THROW(*(it.dbError()));
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::getData
//	METHOD TYPE : void
//
//	DESCRIPTION :
//
void RpmDb::getData( const string & name_r, const Edition & ed_r,
                     RpmHeader::constPtr & result_r ) const
{
  librpmDb::db_const_iterator it;
  it.findPackage( name_r, ed_r  );
  result_r = *it;
  if (it.dbError())
    ZYPP_THROW(*(it.dbError()));
}

///////////////////////////////////////////////////////////////////
//
//	METHOD NAME : RpmDb::checkPackage
//	METHOD TYPE : RpmDb::checkPackageResult
//
RpmDb::checkPackageResult RpmDb::checkPackage( const Pathname & path_r )
{
  PathInfo file( path_r );
  if ( ! file.isFile() )
  {
    ERR << "Not a file: " << file << endl;
    return CHK_ERROR;
  }

  FD_t fd = ::Fopen( file.asString().c_str(), "r.ufdio" );
  if ( fd == 0 || ::Ferror(fd) )
  {
    ERR << "Can't open file for reading: " << file << " (" << ::Fstrerror(fd) << ")" << endl;
    if ( fd )
      ::Fclose( fd );
    return CHK_ERROR;
  }

  rpmts ts = ::rpmtsCreate();
  ::rpmtsSetRootDir( ts, root().asString().c_str() );
  ::rpmtsSetVSFlags( ts, RPMVSF_DEFAULT );
  int res = ::rpmReadPackageFile( ts, fd, path_r.asString().c_str(), NULL );
  ts = rpmtsFree(ts);

  ::Fclose( fd );

  switch ( res )
  {
  case RPMRC_OK:
    return CHK_OK;
    break;
  case RPMRC_NOTFOUND:
    WAR << "Signature is unknown type. " << file << endl;
    return CHK_NOTFOUND;
    break;
  case RPMRC_FAIL:
    WAR << "Signature does not verify. " << file << endl;
    return CHK_FAIL;
    break;
  case RPMRC_NOTTRUSTED:
    WAR << "Signature is OK, but key is not trusted. " << file << endl;
    return CHK_NOTTRUSTED;
    break;
  case RPMRC_NOKEY:
    WAR << "Public key is unavailable. " << file << endl;
    return CHK_NOKEY;
    break;
  }
  ERR << "Error reading header." << file << endl;
  return CHK_ERROR;
}

// determine changed files of installed package
bool
RpmDb::queryChangedFiles(FileList & fileList, const string& packageName)
{
  bool ok = true;

  fileList.clear();

  if ( ! initialized() ) return false;

  RpmArgVec opts;

  opts.push_back ("-V");
  opts.push_back ("--nodeps");
  opts.push_back ("--noscripts");
  opts.push_back ("--nomd5");
  opts.push_back ("--");
  opts.push_back (packageName.c_str());

  run_rpm (opts, ExternalProgram::Discard_Stderr);

  if ( process == NULL )
    return false;

  /* from rpm manpage
   5      MD5 sum
   S      File size
   L      Symlink
   T      Mtime
   D      Device
   U      User
   G      Group
   M      Mode (includes permissions and file type)
  */

  string line;
  while (systemReadLine(line))
  {
    if (line.length() > 12 &&
        (line[0] == 'S' || line[0] == 's' ||
         (line[0] == '.' && line[7] == 'T')))
    {
      // file has been changed
      string filename;

      filename.assign(line, 11, line.length() - 11);
      fileList.insert(filename);
    }
  }

  systemStatus();
  // exit code ignored, rpm returns 1 no matter if package is installed or
  // not

  return ok;
}



/****************************************************************/
/* private member-functions					*/
/****************************************************************/

/*--------------------------------------------------------------*/
/* Run rpm with the specified arguments, handling stderr	*/
/* as specified  by disp					*/
/*--------------------------------------------------------------*/
void
RpmDb::run_rpm (const RpmArgVec& opts,
                ExternalProgram::Stderr_Disposition disp)
{
  if ( process )
  {
    delete process;
    process = NULL;
  }
  exit_code = -1;

  if ( ! initialized() )
  {
    ZYPP_THROW(RpmDbNotOpenException());
  }

  RpmArgVec args;

  // always set root and dbpath
  args.push_back("rpm");
  args.push_back("--root");
  args.push_back(_root.asString().c_str());
  args.push_back("--dbpath");
  args.push_back(_dbPath.asString().c_str());

  const char* argv[args.size() + opts.size() + 1];

  const char** p = argv;
  p = copy (args.begin (), args.end (), p);
  p = copy (opts.begin (), opts.end (), p);
  *p = 0;

  // Invalidate all outstanding database handles in case
  // the database gets modified.
  librpmDb::dbRelease( true );

  // Launch the program with default locale
  process = new ExternalProgram(argv, disp, false, -1, true);
  return;
}

/*--------------------------------------------------------------*/
/* Read a line from the rpm process				*/
/*--------------------------------------------------------------*/
bool RpmDb::systemReadLine( string & line )
{
  line.erase();

  if ( process == NULL )
    return false;

  if ( process->inputFile() )
  {
    process->setBlocking( false );
    FILE * inputfile = process->inputFile();
    int    inputfileFd = ::fileno( inputfile );
    do
    {
      /* Watch inputFile to see when it has input. */
      fd_set rfds;
      FD_ZERO( &rfds );
      FD_SET( inputfileFd, &rfds );

      /* Wait up to 5 seconds. */
      struct timeval tv;
      tv.tv_sec = 5;
      tv.tv_usec = 0;

      int retval = select( inputfileFd+1, &rfds, NULL, NULL, &tv );

      if ( retval == -1 )
      {
	ERR << "select error: " << strerror(errno) << endl;
	if ( errno != EINTR )
	  return false;
      }
      else if ( retval )
      {
	// Data is available now.
	static size_t linebuffer_size = 0;	// static because getline allocs
	static char * linebuffer = 0; 		// and reallocs if buffer is too small
	ssize_t nread = getline( &linebuffer, &linebuffer_size, inputfile );
	if ( nread == -1 )
	{
	  if ( ::feof( inputfile ) )
	    return line.size(); // in case of pending output
	}
	else
	{
	  if ( nread > 0 )
	  {
	    if ( linebuffer[nread-1] == '\n' )
	      --nread;
	    line += string( linebuffer, nread );
	  }

	  if ( ! ::ferror( inputfile ) || ::feof( inputfile ) )
	    return true; // complete line
	}
	clearerr( inputfile );
      }
      else
      {
	// No data within time.
	if ( ! process->running() )
	  return false;
      }
    } while ( true );
  }

  return false;
}

/*--------------------------------------------------------------*/
/* Return the exit status of the rpm process, closing the	*/
/* connection if not already done				*/
/*--------------------------------------------------------------*/
int
RpmDb::systemStatus()
{
  if ( process == NULL )
    return -1;

  exit_code = process->close();
  if (exit_code == 0)
    error_message = "";
  else
    error_message = process->execError();
  process->kill();
  delete process;
  process = 0;

  //   DBG << "exit code " << exit_code << endl;

  return exit_code;
}

/*--------------------------------------------------------------*/
/* Forcably kill the rpm process				*/
/*--------------------------------------------------------------*/
void
RpmDb::systemKill()
{
  if (process) process->kill();
}


// generate diff mails for config files
void RpmDb::processConfigFiles(const string& line, const string& name, const char* typemsg, const char* difffailmsg, const char* diffgenmsg)
{
  string msg = line.substr(9);
  string::size_type pos1 = string::npos;
  string::size_type pos2 = string::npos;
  string file1s, file2s;
  Pathname file1;
  Pathname file2;

  pos1 = msg.find (typemsg);
  for (;;)
  {
    if ( pos1 == string::npos )
      break;

    pos2 = pos1 + strlen (typemsg);

    if (pos2 >= msg.length() )
      break;

    file1 = msg.substr (0, pos1);
    file2 = msg.substr (pos2);

    file1s = file1.asString();
    file2s = file2.asString();

    if (!_root.empty() && _root != "/")
    {
      file1 = _root + file1;
      file2 = _root + file2;
    }

    string out;
    int ret = diffFiles (file1.asString(), file2.asString(), out, 25);
    if (ret)
    {
      Pathname file = _root + WARNINGMAILPATH;
      if (filesystem::assert_dir(file) != 0)
      {
        ERR << "Could not create " << file.asString() << endl;
        break;
      }
      file += Date(Date::now()).form("config_diff_%Y_%m_%d.log");
      ofstream notify(file.asString().c_str(), ios::out|ios::app);
      if (!notify)
      {
        ERR << "Could not open " <<  file << endl;
        break;
      }

      // Translator: %s = name of an rpm package. A list of diffs follows
      // this message.
      notify << str::form(_("Changed configuration files for %s:"), name.c_str()) << endl;
      if (ret>1)
      {
        ERR << "diff failed" << endl;
        notify << str::form(difffailmsg,
                            file1s.c_str(), file2s.c_str()) << endl;
      }
      else
      {
        notify << str::form(diffgenmsg,
                            file1s.c_str(), file2s.c_str()) << endl;

        // remove root for the viewer's pleasure (#38240)
        if (!_root.empty() && _root != "/")
        {
          if (out.substr(0,4) == "--- ")
          {
            out.replace(4, file1.asString().length(), file1s);
          }
          string::size_type pos = out.find("\n+++ ");
          if (pos != string::npos)
          {
            out.replace(pos+5, file2.asString().length(), file2s);
          }
        }
        notify << out << endl;
      }
      notify.close();
      notify.open("/var/lib/update-messages/yast2-packagemanager.rpmdb.configfiles");
      notify.close();
    }
    else
    {
      WAR << "rpm created " << file2 << " but it is not different from " << file2 << endl;
    }
    break;
  }
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::installPackage
//	METHOD TYPE : PMError
//
void RpmDb::installPackage( const Pathname & filename, RpmInstFlags flags )
{
  callback::SendReport<RpmInstallReport> report;

  report->start(filename);

  do
    try
    {
      doInstallPackage(filename, flags, report);
      report->finish();
      break;
    }
    catch (RpmException & excpt_r)
    {
      RpmInstallReport::Action user = report->problem( excpt_r );

      if ( user == RpmInstallReport::ABORT )
      {
        report->finish( excpt_r );
        ZYPP_RETHROW(excpt_r);
      }
      else if ( user == RpmInstallReport::IGNORE )
      {
        break;
      }
    }
  while (true);
}

void RpmDb::doInstallPackage( const Pathname & filename, RpmInstFlags flags, callback::SendReport<RpmInstallReport> & report )
{
  FAILIFNOTINITIALIZED;
  HistoryLog historylog;

  MIL << "RpmDb::installPackage(" << filename << "," << flags << ")" << endl;


  // backup
  if ( _packagebackups )
  {
    // FIXME      report->progress( pd.init( -2, 100 ) ); // allow 1% for backup creation.
    if ( ! backupPackage( filename ) )
    {
      ERR << "backup of " << filename.asString() << " failed" << endl;
    }
    // FIXME status handling
    report->progress( 0 ); // allow 1% for backup creation.
  }

  // run rpm
  RpmArgVec opts;
  if (flags & RPMINST_NOUPGRADE)
    opts.push_back("-i");
  else
    opts.push_back("-U");

  opts.push_back("--percent");

  // ZConfig defines cross-arch installation
  if ( ! ZConfig::instance().systemArchitecture().compatibleWith( ZConfig::instance().defaultSystemArchitecture() ) )
    opts.push_back("--ignorearch");

  if (flags & RPMINST_NODIGEST)
    opts.push_back("--nodigest");
  if (flags & RPMINST_NOSIGNATURE)
    opts.push_back("--nosignature");
  if (flags & RPMINST_EXCLUDEDOCS)
    opts.push_back ("--excludedocs");
  if (flags & RPMINST_NOSCRIPTS)
    opts.push_back ("--noscripts");
  if (flags & RPMINST_FORCE)
    opts.push_back ("--force");
  if (flags & RPMINST_NODEPS)
    opts.push_back ("--nodeps");
  if (flags & RPMINST_IGNORESIZE)
    opts.push_back ("--ignoresize");
  if (flags & RPMINST_JUSTDB)
    opts.push_back ("--justdb");
  if (flags & RPMINST_TEST)
    opts.push_back ("--test");

  opts.push_back("--");

  // rpm requires additional quoting of special chars:
  string quotedFilename( rpmQuoteFilename( filename ) );
  opts.push_back ( quotedFilename.c_str() );

  modifyDatabase(); // BEFORE run_rpm
  run_rpm( opts, ExternalProgram::Stderr_To_Stdout );

  string line;
  string rpmmsg;
  vector<string> configwarnings;

  unsigned linecnt = 0;
  while (systemReadLine(line))
  {
    if ( linecnt < MAXRPMMESSAGELINES )
      ++linecnt;
    else
      continue;

    if (line.substr(0,2)=="%%")
    {
      int percent;
      sscanf (line.c_str () + 2, "%d", &percent);
      report->progress( percent );
    }
    else
      rpmmsg += line+'\n';

    if ( line.substr(0,8) == "warning:" )
    {
      configwarnings.push_back(line);
    }
  }
  if ( linecnt > MAXRPMMESSAGELINES )
    rpmmsg += "[truncated]\n";

  int rpm_status = systemStatus();

  // evaluate result
  for (vector<string>::iterator it = configwarnings.begin();
       it != configwarnings.end(); ++it)
  {
    processConfigFiles(*it, Pathname::basename(filename), " saved as ",
                       // %s = filenames
                       _("rpm saved %s as %s, but it was impossible to determine the difference"),
                       // %s = filenames
                       _("rpm saved %s as %s.\nHere are the first 25 lines of difference:\n"));
    processConfigFiles(*it, Pathname::basename(filename), " created as ",
                       // %s = filenames
                       _("rpm created %s as %s, but it was impossible to determine the difference"),
                       // %s = filenames
                       _("rpm created %s as %s.\nHere are the first 25 lines of difference:\n"));
  }

  if ( rpm_status != 0 )
  {
    historylog.comment(
        str::form("%s install failed", Pathname::basename(filename).c_str()),
        true /*timestamp*/);
    ostringstream sstr;
    sstr << "rpm output:" << endl << rpmmsg << endl;
    historylog.comment(sstr.str());
    // TranslatorExplanation the colon is followed by an error message
    ZYPP_THROW(RpmSubprocessException(string(_("RPM failed: ")) +
               (rpmmsg.empty() ? error_message : rpmmsg)));
  }
  else if ( ! rpmmsg.empty() )
  {
    historylog.comment(
        str::form("%s installed ok", Pathname::basename(filename).c_str()),
        true /*timestamp*/);
    ostringstream sstr;
    sstr << "Additional rpm output:" << endl << rpmmsg << endl;
    historylog.comment(sstr.str());

    // report additional rpm output in finish
    // TranslatorExplanation Text is followed by a ':'  and the actual output.
    report->finishInfo(str::form( "%s:\n%s\n", _("Additional rpm output"),  rpmmsg.c_str() ));
  }
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::removePackage
//	METHOD TYPE : PMError
//
void RpmDb::removePackage( Package::constPtr package, RpmInstFlags flags )
{
  // 'rpm -e' does not like epochs
  return removePackage( package->name()
                        + "-" + package->edition().version()
                        + "-" + package->edition().release()
                        + "." + package->arch().asString(), flags );
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::removePackage
//	METHOD TYPE : PMError
//
void RpmDb::removePackage( const string & name_r, RpmInstFlags flags )
{
  callback::SendReport<RpmRemoveReport> report;

  report->start( name_r );

  do
    try
    {
      doRemovePackage(name_r, flags, report);
      report->finish();
      break;
    }
    catch (RpmException & excpt_r)
    {
      RpmRemoveReport::Action user = report->problem( excpt_r );

      if ( user == RpmRemoveReport::ABORT )
      {
        report->finish( excpt_r );
        ZYPP_RETHROW(excpt_r);
      }
      else if ( user == RpmRemoveReport::IGNORE )
      {
        break;
      }
    }
  while (true);
}


void RpmDb::doRemovePackage( const string & name_r, RpmInstFlags flags, callback::SendReport<RpmRemoveReport> & report )
{
  FAILIFNOTINITIALIZED;
  HistoryLog historylog;

  MIL << "RpmDb::doRemovePackage(" << name_r << "," << flags << ")" << endl;

  // backup
  if ( _packagebackups )
  {
    // FIXME solve this status report somehow
    //      report->progress( pd.init( -2, 100 ) ); // allow 1% for backup creation.
    if ( ! backupPackage( name_r ) )
    {
      ERR << "backup of " << name_r << " failed" << endl;
    }
    report->progress( 0 );
  }
  else
  {
    report->progress( 100 );
  }

  // run rpm
  RpmArgVec opts;
  opts.push_back("-e");
  opts.push_back("--allmatches");

  if (flags & RPMINST_NOSCRIPTS)
    opts.push_back("--noscripts");
  if (flags & RPMINST_NODEPS)
    opts.push_back("--nodeps");
  if (flags & RPMINST_JUSTDB)
    opts.push_back("--justdb");
  if (flags & RPMINST_TEST)
    opts.push_back ("--test");
  if (flags & RPMINST_FORCE)
  {
    WAR << "IGNORE OPTION: 'rpm -e' does not support '--force'" << endl;
  }

  opts.push_back("--");
  opts.push_back(name_r.c_str());

  modifyDatabase(); // BEFORE run_rpm
  run_rpm (opts, ExternalProgram::Stderr_To_Stdout);

  string line;
  string rpmmsg;

  // got no progress from command, so we fake it:
  // 5  - command started
  // 50 - command completed
  // 100 if no error
  report->progress( 5 );
  unsigned linecnt = 0;
  while (systemReadLine(line))
  {
    if ( linecnt < MAXRPMMESSAGELINES )
      ++linecnt;
    else
      continue;
    rpmmsg += line+'\n';
  }
  if ( linecnt > MAXRPMMESSAGELINES )
    rpmmsg += "[truncated]\n";
  report->progress( 50 );
  int rpm_status = systemStatus();

  if ( rpm_status != 0 )
  {
    historylog.comment(
        str::form("%s remove failed", name_r.c_str()), true /*timestamp*/);
    ostringstream sstr;
    sstr << "rpm output:" << endl << rpmmsg << endl;
    historylog.comment(sstr.str());
    // TranslatorExplanation the colon is followed by an error message
    ZYPP_THROW(RpmSubprocessException(string(_("RPM failed: ")) +
               (rpmmsg.empty() ? error_message: rpmmsg)));
  }
  else if ( ! rpmmsg.empty() )
  {
    historylog.comment(
        str::form("%s removed ok", name_r.c_str()), true /*timestamp*/);

    ostringstream sstr;
    sstr << "Additional rpm output:" << endl << rpmmsg << endl;
    historylog.comment(sstr.str());

    // report additional rpm output in finish
    // TranslatorExplanation Text is followed by a ':'  and the actual output.
    report->finishInfo(str::form( "%s:\n%s\n", _("Additional rpm output"),  rpmmsg.c_str() ));
  }
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::backupPackage
//	METHOD TYPE : bool
//
bool RpmDb::backupPackage( const Pathname & filename )
{
  RpmHeader::constPtr h( RpmHeader::readPackage( filename, RpmHeader::NOSIGNATURE ) );
  if ( ! h )
    return false;

  return backupPackage( h->tag_name() );
}

///////////////////////////////////////////////////////////////////
//
//
//	METHOD NAME : RpmDb::backupPackage
//	METHOD TYPE : bool
//
bool RpmDb::backupPackage(const string& packageName)
{
  HistoryLog progresslog;
  bool ret = true;
  Pathname backupFilename;
  Pathname filestobackupfile = _root+_backuppath+FILEFORBACKUPFILES;

  if (_backuppath.empty())
  {
    INT << "_backuppath empty" << endl;
    return false;
  }

  FileList fileList;

  if (!queryChangedFiles(fileList, packageName))
  {
    ERR << "Error while getting changed files for package " <<
    packageName << endl;
    return false;
  }

  if (fileList.size() <= 0)
  {
    DBG <<  "package " <<  packageName << " not changed -> no backup" << endl;
    return true;
  }

  if (filesystem::assert_dir(_root + _backuppath) != 0)
  {
    return false;
  }

  {
    // build up archive name
    time_t currentTime = time(0);
    struct tm *currentLocalTime = localtime(&currentTime);

    int date = (currentLocalTime->tm_year + 1900) * 10000
               + (currentLocalTime->tm_mon + 1) * 100
               + currentLocalTime->tm_mday;

    int num = 0;
    do
    {
      backupFilename = _root + _backuppath
                       + str::form("%s-%d-%d.tar.gz",packageName.c_str(), date, num);

    }
    while ( PathInfo(backupFilename).isExist() && num++ < 1000);

    PathInfo pi(filestobackupfile);
    if (pi.isExist() && !pi.isFile())
    {
      ERR << filestobackupfile.asString() << " already exists and is no file" << endl;
      return false;
    }

    ofstream fp ( filestobackupfile.asString().c_str(), ios::out|ios::trunc );

    if (!fp)
    {
      ERR << "could not open " << filestobackupfile.asString() << endl;
      return false;
    }

    for (FileList::const_iterator cit = fileList.begin();
         cit != fileList.end(); ++cit)
    {
      string name = *cit;
      if ( name[0] == '/' )
      {
        // remove slash, file must be relative to -C parameter of tar
        name = name.substr( 1 );
      }
      DBG << "saving file "<< name << endl;
      fp << name << endl;
    }
    fp.close();

    const char* const argv[] =
      {
        "tar",
        "-czhP",
        "-C",
        _root.asString().c_str(),
        "--ignore-failed-read",
        "-f",
        backupFilename.asString().c_str(),
        "-T",
        filestobackupfile.asString().c_str(),
        NULL
      };

    // execute tar in inst-sys (we dont know if there is a tar below _root !)
    ExternalProgram tar(argv, ExternalProgram::Stderr_To_Stdout, false, -1, true);

    string tarmsg;

    // TODO: its probably possible to start tar with -v and watch it adding
    // files to report progress
    for (string output = tar.receiveLine(); output.length() ;output = tar.receiveLine())
    {
      tarmsg+=output;
    }

    int ret = tar.close();

    if ( ret != 0)
    {
      ERR << "tar failed: " << tarmsg << endl;
      ret = false;
    }
    else
    {
      MIL << "tar backup ok" << endl;
      progresslog.comment(
          str::form(_("created backup %s"), backupFilename.asString().c_str())
          , /*timestamp*/true);
    }

    filesystem::unlink(filestobackupfile);
  }

  return ret;
}

void RpmDb::setBackupPath(const Pathname& path)
{
  _backuppath = path;
}

} // namespace rpm
} // namespace target
} // namespace zypp
