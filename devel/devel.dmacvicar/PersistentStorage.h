/*---------------------------------------------------------------------\
|                          ____ _   __ __ ___                          |
|                         |__  / \ / / . \ . \                         |
|                           / / \ V /|  _/  _/                         |
|                          / /__ | | | | | |                           |
|                         /_____||_| |_| |_|                           |
|                                                                      |
\---------------------------------------------------------------------*/
/** \file	devel/devel.dmacvicar/PersistentStorage.h
*
*/
#ifndef DEVEL_DEVEL_DMACVICAR_PERSISTENTSTORAGE_H
#define DEVEL_DEVEL_DMACVICAR_PERSISTENTSTORAGE_H

#include <iosfwd>

#include "zypp/base/ReferenceCounted.h"
#include "zypp/base/NonCopyable.h"
#include "zypp/base/PtrTypes.h"
#include <zypp/Patch.h>

///////////////////////////////////////////////////////////////////
namespace zypp
{ /////////////////////////////////////////////////////////////////
  ///////////////////////////////////////////////////////////////////
  namespace storage
  { /////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////
    //
    //	CLASS NAME : PersistentStorage
    //
    /** */
    class PersistentStorage : public base::ReferenceCounted, private base::NonCopyable
    {
      friend std::ostream & operator<<( std::ostream & str, const PersistentStorage & obj );
      typedef intrusive_ptr<PersistentStorage> Ptr;
      typedef intrusive_ptr<const PersistentStorage> constPtr;
    public:
      /** Default ctor */
      PersistentStorage();
      /** Dtor */
      ~PersistentStorage();
      void doTest();

    public:
      /**
       * Stores a Resolvable in the active backend.
       */
      void storeObject( Resolvable::Ptr resolvable );
      /**
       * Deletes a Resolvable from the active backend.
       */
      void deleteObject( Resolvable::Ptr resolvable );
      /**
       * Query for installed Resolvables.
       */
      std::list<Resolvable::Ptr> storedObjects();
       /**
       * Query for installed Resolvables of a certain kind.
       */
      std::list<Resolvable::Ptr> storedObjects(const Resolvable::Kind kind);
       /**
       * Query for installed Resolvables of a certain kind by name
       * \a partial_match allows for text search.
       */
      std::list<Resolvable::Ptr> storedObjects(const Resolvable::Kind kind, const std::string & name, bool partial_match = false);

    private:
      class Private;
      Private *d;
    };
    ///////////////////////////////////////////////////////////////////
    /** \relates PersistentStorage Stream output */
    std::ostream & operator<<( std::ostream & str, const PersistentStorage & obj );

    /////////////////////////////////////////////////////////////////
  } // namespace devel.dmacvicar
  ///////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////
} // namespace devel
///////////////////////////////////////////////////////////////////
#endif // DEVEL_DEVEL_DMACVICAR_PERSISTENTSTORAGE_H
