/*---------------------------------------------------------------------\
|                          ____ _   __ __ ___                          |
|                         |__  / \ / / . \ . \                         |
|                           / / \ V /|  _/  _/                         |
|                          / /__ | | | | | |                           |
|                         /_____||_| |_| |_|                           |
|                                                                      |
\---------------------------------------------------------------------*/
/** \file zypp/media/ProxyInfo.h
 *
*/
#ifndef ZYPP_MEDIA_PROXYINFO_H
#define ZYPP_MEDIA_PROXYINFO_H

#include <string>
#include <list>

#include "zypp/base/PtrTypes.h"

namespace zypp {
  namespace media {

    ///////////////////////////////////////////////////////////////////
    //
    //	CLASS NAME : ProxyInfo
    class ProxyInfo
    {
    public:
      typedef intrusive_ptr<ProxyInfo> Ptr;
      typedef intrusive_ptr<ProxyInfo> constPtr;
      typedef std::list<std::string> NoProxyList;
      typedef std::list<std::string>::const_iterator NoProxyIterator;
      
      /** Implementation */
      struct Impl;
      /** Ctor */
      ProxyInfo();
      /** Ctor */
      ProxyInfo(RW_pointer<Impl> impl);
      bool enabled() const;
      std::string proxy(const std::string & protocol_r) const;
      NoProxyList noProxy() const;
      NoProxyIterator noProxyBegin() const;
      NoProxyIterator noProxyEnd() const;
    private:
      /** Pointer to implementation */
      RW_pointer<Impl> _pimpl;
    };


///////////////////////////////////////////////////////////////////

  } // namespace media
} // namespace zypp

#endif // ZYPP_MEDIA_PROXYINFO_H
