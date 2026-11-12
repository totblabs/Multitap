// The configured HTTP origin is distinct from a TLS reverse proxy's private
// HTTP backend listener. No hostname or address proves internal-only exposure.
export function startupSecurityWarnings(config) {
  const access = config.accessConfig;
  if (config.bootstrapOnly || access?.http?.enabled !== true) return [];
  return [{
    service: 'multitap-broker',
    event: 'security_warning',
    code: 'cleartext_http_enabled',
    scope: 'configured_http_endpoint',
    originRole: access.primaryOrigin === access.http.origin ? 'primary' : 'additional',
    exposure: 'not_inferred',
    credentialsAndSessionDataMayBePlaintext: true,
  }];
}
