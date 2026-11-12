#pragma once
#include <string_view>

namespace multitap {
// The console page asks the window's close question in its own dialog.
// The window sends only this request; the page answers only
// with the strings below. Anything else is ignored.
inline constexpr wchar_t kConsoleCloseRequest[]=L"multitap-console:close-request";
enum class ConsolePageMessage{Ignored,Ready,Shown,Confirm,Cancel};
inline ConsolePageMessage ParseConsolePageMessage(std::wstring_view text){
  if(text==L"multitap-console:ready")return ConsolePageMessage::Ready;
  if(text==L"multitap-console:close-shown")return ConsolePageMessage::Shown;
  if(text==L"multitap-console:close")return ConsolePageMessage::Confirm;
  if(text==L"multitap-console:close-cancel")return ConsolePageMessage::Cancel;
  return ConsolePageMessage::Ignored;
}
// What the window does about a close (X, Alt+F4, the system menu). It never
// traps the user: a page that has not said it can ask (an older broker, a page
// still loading), that cannot take the request or that does not answer in
// time gets the native question instead.
struct ConsoleCloseFlow {
  enum class Step{None,AskPage,AskNative,Close,Answered};
  bool pageReady=false,asked=false;
  Step Request(){if(!pageReady)return Step::AskNative;asked=true;return Step::AskPage;}
  // The request could not be posted, or no answer came in time: the page is
  // not asked again until a new document says it can.
  Step Unanswered(){if(!asked)return Step::None;asked=false;pageReady=false;return Step::AskNative;}
  Step Message(ConsolePageMessage message){
    switch(message){
      case ConsolePageMessage::Ready:pageReady=true;return Step::None;
      case ConsolePageMessage::Shown:return asked?Step::Answered:Step::None;
      // Only an answer to an open request closes the window.
      case ConsolePageMessage::Confirm:if(!asked)return Step::None;asked=false;return Step::Close;
      case ConsolePageMessage::Cancel:if(!asked)return Step::None;asked=false;return Step::Answered;
      default:return Step::None;
    }
  }
  // A new document must say again that it can ask.
  void Navigated(){pageReady=asked=false;}
};
}
