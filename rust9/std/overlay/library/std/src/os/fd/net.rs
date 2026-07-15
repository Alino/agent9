use crate::os::fd::owned::OwnedFd;
use crate::os::fd::raw::{AsRawFd, FromRawFd, IntoRawFd, RawFd};
use crate::sys::{AsInner, FromInner, IntoInner};
use crate::{net, sys};

macro_rules! impl_as_raw_fd {
    ($($t:ident)*) => {$(
        #[stable(feature = "rust1", since = "1.0.0")]
        impl AsRawFd for net::$t {
            #[inline]
            fn as_raw_fd(&self) -> RawFd {
                self.as_inner().socket().as_raw_fd()
            }
        }
    )*};
}
impl_as_raw_fd! { TcpStream TcpListener UdpSocket }

macro_rules! impl_from_raw_fd {
    ($($t:ident)*) => {$(
        #[stable(feature = "from_raw_os", since = "1.1.0")]
        impl FromRawFd for net::$t {
            #[inline]
            unsafe fn from_raw_fd(fd: RawFd) -> net::$t {
                unsafe {
                    let socket = sys::net::Socket::from_inner(FromInner::from_inner(OwnedFd::from_raw_fd(fd)));
                    net::$t::from_inner(sys::net::$t::from_inner(socket))
                }
            }
        }
    )*};
}
#[cfg(not(target_os = "plan9"))]
impl_from_raw_fd! { TcpStream TcpListener UdpSocket }

// plan9: a connection is one fd plus a conn directory the fd can name, so
// rebuilding from a bare fd means asking fd2path(2) where it points and reading
// the peer back out of /net/tcp/N/remote. Not the Berkeley one-liner, but real.
//
// TcpListener/UdpSocket are NOT provided: a listener's identity is its
// announcement (its ctl fd), which a data fd cannot reconstruct. Callers that
// need those should keep the typed value rather than round-trip through a fd.
#[cfg(target_os = "plan9")]
#[stable(feature = "from_raw_os", since = "1.1.0")]
impl FromRawFd for net::TcpStream {
    #[inline]
    unsafe fn from_raw_fd(fd: RawFd) -> net::TcpStream {
        let inner = unsafe { sys::net::TcpStream::from_raw_fd_plan9(fd) }
            .expect("from_raw_fd: not an open fd under /net");
        net::TcpStream::from_inner(inner)
    }
}

#[cfg(target_os = "plan9")]
#[stable(feature = "from_raw_os", since = "1.1.0")]
impl FromRawFd for net::TcpListener {
    #[inline]
    unsafe fn from_raw_fd(fd: RawFd) -> net::TcpListener {
        let inner = unsafe { sys::net::TcpListener::from_raw_fd_plan9(fd) }
            .expect("from_raw_fd: not an open /net announce ctl fd");
        net::TcpListener::from_inner(inner)
    }
}

#[cfg(target_os = "plan9")]
#[stable(feature = "from_raw_os", since = "1.1.0")]
impl FromRawFd for net::UdpSocket {
    #[inline]
    unsafe fn from_raw_fd(fd: RawFd) -> net::UdpSocket {
        let inner = unsafe { sys::net::UdpSocket::from_raw_fd_plan9(fd) }
            .expect("from_raw_fd: not an open /net/udp data fd");
        net::UdpSocket::from_inner(inner)
    }
}

macro_rules! impl_into_raw_fd {
    ($($t:ident)*) => {$(
        #[stable(feature = "into_raw_os", since = "1.4.0")]
        impl IntoRawFd for net::$t {
            #[inline]
            fn into_raw_fd(self) -> RawFd {
                self.into_inner().into_socket().into_inner().into_inner().into_raw_fd()
            }
        }
    )*};
}
#[cfg(not(target_os = "plan9"))]
impl_into_raw_fd! { TcpStream TcpListener UdpSocket }

// plan9: hand out the data fd and forget the wrapper, so the caller owns it.
#[cfg(target_os = "plan9")]
macro_rules! impl_into_raw_fd_plan9 {
    ($($t:ident)*) => {$(
        #[stable(feature = "into_raw_os", since = "1.4.0")]
        impl IntoRawFd for net::$t {
            #[inline]
            fn into_raw_fd(self) -> RawFd {
                // Hand out the fd and forget the wrapper so the caller owns it.
                // NB: for UdpSocket this leaks the ctl fd — the conn dir stays
                // alive via data, and from_raw_fd reopens ctl, so the round-trip
                // is sound; a plain into_raw_fd with no matching from_raw_fd
                // strands one fd. Acceptable, and no worse than the alternative
                // of hanging up a socket the caller still owns.
                let fd = self.as_raw_fd();
                crate::mem::forget(self);
                fd
            }
        }
    )*};
}
#[cfg(target_os = "plan9")]
impl_into_raw_fd_plan9! { TcpStream TcpListener UdpSocket }
