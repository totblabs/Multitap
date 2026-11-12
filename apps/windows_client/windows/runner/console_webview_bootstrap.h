#pragma once
#include <WebView2.h>
#include "console_webview_policy.h"
namespace multitap {
// Shared by the production view and private no-guest bootstrap probe.
HRESULT NavigateConsoleBootstrap(ICoreWebView2Environment* environment,
                                ICoreWebView2* view,ConsoleWebViewTarget& target);
// Chromium's initial opaque-document Origin can replace navigation headers.
// Call only from WebResourceRequested, before dispatching the owned POST.
inline HRESULT RestoreConsoleBootstrapOrigin(ICoreWebView2WebResourceRequest* request,
    const ConsoleWebViewTarget& target,COREWEBVIEW2_WEB_RESOURCE_CONTEXT context,bool& handled){
  if(!request)return E_POINTER;
  LPWSTR rawUri=nullptr,rawMethod=nullptr;
  auto result=request->get_Uri(&rawUri);if(SUCCEEDED(result))result=request->get_Method(&rawMethod);
  std::string uri,method;
  const auto copy=[](PCWSTR value,std::string& out,size_t limit){if(!value)return false;for(;*value;++value){if(out.size()>=limit||*value<0x20||*value>0x7e)return false;out+=char(*value);}return true;};
  if(SUCCEEDED(result)&&(!copy(rawUri,uri,4096)||!copy(rawMethod,method,16)))result=E_INVALIDARG;
  CoTaskMemFree(rawUri);CoTaskMemFree(rawMethod);
  if(FAILED(result))return result;
  if(!ConsoleWebViewAttachPost(target,uri,method,!handled,context==COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT))return S_FALSE;
  ICoreWebView2HttpRequestHeaders* headers=nullptr;result=request->get_Headers(&headers);
  if(SUCCEEDED(result)&&headers){const std::wstring origin(target.origin.begin(),target.origin.end());result=headers->SetHeader(L"Origin",origin.c_str());}
  else if(SUCCEEDED(result))result=E_FAIL;
  if(headers)headers->Release();if(SUCCEEDED(result))handled=true;return result;
}
}
