#pragma once
#include "machine.hpp"
namespace host {
inline std::string nativeBaselineDigest(const std::string& bytes){
  if(bytes.size()>60000)throw std::runtime_error("native_baseline_invalid");BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;check(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0),"Native baseline hash provider");
  // CNG keeps references into the caller-provided hash object until
  // BCryptDestroyHash. Declare its backing storage before the RAII cleanup so
  // both success and exception unwinding destroy the handle before the bytes.
  std::vector<BYTE> object;
  struct Cleanup{BCRYPT_ALG_HANDLE algorithm;BCRYPT_HASH_HANDLE* hash;~Cleanup(){if(*hash)BCryptDestroyHash(*hash);BCryptCloseAlgorithmProvider(algorithm,0);}}cleanup{algorithm,&hash};DWORD size=0,got=0;check(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&got,0),"Native baseline hash size");object.resize(size);check(BCryptCreateHash(algorithm,&hash,object.data(),size,nullptr,0,0),"Native baseline hash create");check(BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),ULONG(bytes.size()),0),"Native baseline hash data");std::array<BYTE,32> digest{};check(BCryptFinishHash(hash,digest.data(),DWORD(digest.size()),0),"Native baseline hash finish");const char hex[]="0123456789abcdef";std::string out;for(const auto byte:digest){out+=hex[byte>>4];out+=hex[byte&15];}return out;
}
}
