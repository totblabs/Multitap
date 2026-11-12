package me.totb.multitap_client

fun main() {
    check(AndroidTvPolicy.hardwareKeyboard(true, false))
    check(!AndroidTvPolicy.hardwareKeyboard(true, true))
    check(!AndroidTvPolicy.hardwareKeyboard(false, false))
    check(AndroidTvPolicy.television(4, false))
    check(AndroidTvPolicy.television(1, true))
    check(!AndroidTvPolicy.television(1, false))
    for (key in listOf(19, 20, 21, 22, 23, 66, 82)) {
        check(AndroidTvPolicy.flutterNavigation(true, false, key))
        check(!AndroidTvPolicy.flutterNavigation(true, true, key))
        check(!AndroidTvPolicy.flutterNavigation(false, false, key))
    }
    for (key in listOf(3, 4, 24, 25, 26, 29, 61, 111)) {
        check(!AndroidTvPolicy.flutterNavigation(true, false, key))
    }
    println("AndroidTvPolicyCheck passed: TV/phone, remote/QWERTY, navigation and local system keys")
}
