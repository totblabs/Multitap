package me.totb.multitap_client

fun main() {
    val origin = "https://fixture.invalid:8443"
    val target = ConsoleWebViewPolicy.target(origin, "pve-149", "/console/pve-149/attach")!!
    check(target.consoleUrl == "$origin/console/pve-149")
    check(target.attachUrl == "$origin/console/pve-149/attach")
    check(ConsoleWebViewPolicy.code("c".repeat(43)))
    check(ConsoleWebViewPolicy.ownerId("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
    check(ConsoleWebViewPolicy.ownerId("c".repeat(43)) != ConsoleWebViewPolicy.ownerId("d".repeat(43)))
    for (code in listOf("", "c".repeat(42), "c".repeat(44), "!".repeat(43))) check(!ConsoleWebViewPolicy.code(code))
    for (bad in listOf("http://fixture.invalid", "$origin/", "$origin/path", "$origin?token=secret", "https://user:secret@fixture.invalid", "https://fixture.invalid:0", "https://fixture.invalid:65536")) check(ConsoleWebViewPolicy.target(bad, "pve-149", "/console/pve-149/attach") == null)
    for (id in listOf("../other", "pve-149/other", "_x", "x%2fy", "x?secret", "a".repeat(81))) check(ConsoleWebViewPolicy.target(origin, id, "/console/$id/attach") == null)
    for (path in listOf("/console/pve-150/attach", "https://outside.invalid/console/pve-149/attach", "//outside.invalid/attach", "/console/pve-149/attach?code=secret")) check(ConsoleWebViewPolicy.target(origin, "pve-149", path) == null)
    check(target.navigation(target.consoleUrl))
    for (url in listOf("https://outside.invalid/console/pve-149", "$origin/console/pve-150", "$origin/console/pve-149?code=secret", "$origin/console/pve-149#secret", "$origin/console/pve-149/attach", "file:///console/pve-149", "content://console/pve-149", "javascript:alert(1)", "intent://fixture.invalid/console/pve-149", "http://fixture.invalid:8443/console/pve-149")) check(!target.navigation(url))
    for (path in listOf("/console/assets/fixture/client.js", "/console/api/session", "/console/api/logout", "/console/api/desktops/pve-149/connect", "/console/api/desktops/pve-149/sessions/01234567-89ab-cdef-8123-456789abcdef")) check(target.resource(origin + path))
    for (path in listOf("/admin/", "/console/api/login", "/console/api/mfa/enable", "/console/assets/../admin.js", "/console/assets/%2e%2e/admin.js", "/console/api/desktops/pve-150/connect", "/console/api/session?code=secret")) check(!target.resource(origin + path))
    val cookie = "${target.cookieName}=${"c".repeat(43)}; Path=/console; HttpOnly; Secure; SameSite=Strict"
    check(target.cookie(cookie) == cookie)
    for (bad in listOf(cookie.replace("HttpOnly", "Domain=fixture.invalid"), cookie.replace("Secure;", ""), cookie.replace("SameSite=Strict", "SameSite=None"), cookie.replace("Path=/console", "Path=/"), "$cookie; Domain=fixture.invalid", "$cookie; Max-Age=3600", "$cookie; Secure", cookie.replace(target.cookieName, "admin"), "$cookie\r\nInjected:yes")) check(target.cookie(bad) == null)
    check(target.cookieName != ConsoleWebViewPolicy.target("https://fixture.invalid", "pve-149", "/console/pve-149/attach")!!.cookieName)
    println("ConsoleWebViewPolicyCheck passed: same-origin attachment/navigation, child-cookie isolation and unsafe resource rejection")
}
