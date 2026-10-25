//  (C) Copyright John Maddock 2001 - 2003. 
//  (C) Copyright David Abrahams 2002 - 2003. 
//  Use, modification and distribution are subject to the 
//  Boost Software License, Version 1.0. (See accompanying file 
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_CONFIG_PLATFORM_WIN32_HPP
#define BOOST_CONFIG_PLATFORM_WIN32_HPP

#define BOOST_PLATFORM "Win32"

#if defined(__GNUC__)
#  define BOOST_NO_SWPRINTF
#endif

#if !defined(__MINGW32__) && !defined(_MSC_VER)
#  define BOOST_HAS_STDINT_H
#endif

#if defined(__MINGW32__)
#  define BOOST_HAS_STDINT_H
#  define BOOST_HAS_DIRENT_H
#  define BOOST_HAS_UNISTD_H
#endif

#endif // BOOST_CONFIG_PLATFORM_WIN32_HPP
