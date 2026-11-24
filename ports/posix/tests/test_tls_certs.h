/* SPDX-License-Identifier: Apache-2.0
 *
 * Test certificates for test_tls.c, valid until 2126. Generated once with
 * openssl (P-256):
 *   tenant_ca  CN=tiny-edge test tenant CA     (stands for the tenant's root)
 *   dl_ca      CN=tiny-edge test dl CA         (a download-only root)
 *   srv_cert   CN=download.test, issued by dl_ca, SAN DNS:download.test
 *   lh_cert    CN=localhost, issued by dl_ca, SAN DNS:localhost (a fake tenant)
 * Test-only: the private keys below protect nothing.
 */

#ifndef TEST_TLS_CERTS_H_
#define TEST_TLS_CERTS_H_

static const char tenant_ca[] =
	"-----BEGIN CERTIFICATE-----\n"
	"MIIBjDCCATKgAwIBAgIUFNrovHY//j6a8RoP33Ub8pnGlpIwCgYIKoZIzj0EAwIw\n"
	"IzEhMB8GA1UEAwwYdGlueS1lZGdlIHRlc3QgdGVuYW50IENBMCAXDTI2MTEyNDAy\n"
	"MjE1NloYDzIxMjYxMDMxMDIyMTU2WjAjMSEwHwYDVQQDDBh0aW55LWVkZ2UgdGVz\n"
	"dCB0ZW5hbnQgQ0EwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAAQQiFB7NSSoLunc\n"
	"KBDg6USPn0Qh7gaf/36xDhMNwmo2oGyg5FOV94NHAlP/cd0ZtsjqkV3GfkbAZNZq\n"
	"CJmjUzAZo0IwQDAdBgNVHQ4EFgQU0CA60l5eG2hI1NBfN+A0ICEJmhUwDwYDVR0T\n"
	"AQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAQYwCgYIKoZIzj0EAwIDSAAwRQIgYRS0\n"
	"t46y2LG/ia2OUe10Vn+zfgcyAKqaUJo8I8YCO0ICIQCLQgDUWT7lLHjbCDrFeLRR\n"
	"TqW2gTJ+T72qcXLJ4S+oGg==\n"
	"-----END CERTIFICATE-----\n";

static const char dl_ca[] =
	"-----BEGIN CERTIFICATE-----\n"
	"MIIBhDCCASqgAwIBAgIUbe2xVaRTs+ivoOf7kdg0R+G/DrswCgYIKoZIzj0EAwIw\n"
	"HzEdMBsGA1UEAwwUdGlueS1lZGdlIHRlc3QgZGwgQ0EwIBcNMjYxMTI0MDIyMTU2\n"
	"WhgPMjEyNjEwMzEwMjIxNTZaMB8xHTAbBgNVBAMMFHRpbnktZWRnZSB0ZXN0IGRs\n"
	"IENBMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE+Abwfg6zneJla7WlP3l1LQ9S\n"
	"cN+nyvqBRHTH7sj5GJ33TXeH19SoScE15tm2T+DqP0DQbjMl94isvGmqbnb2MKNC\n"
	"MEAwHQYDVR0OBBYEFB5dHSky7ZALRVDAlhCTD5ssa2UVMA8GA1UdEwEB/wQFMAMB\n"
	"Af8wDgYDVR0PAQH/BAQDAgEGMAoGCCqGSM49BAMCA0gAMEUCIQCyIqaKNfejzh/o\n"
	"airAk2z8h3gZVWCBX6huKE9BU9waFgIgaYBwXAMLVI0fo8NWvnURJFkf28d2VomV\n"
	"MkdWS4FHeGM=\n"
	"-----END CERTIFICATE-----\n";

static const char srv_cert[] =
	"-----BEGIN CERTIFICATE-----\n"
	"MIIBdzCCAR2gAwIBAgIUOUNEJy73cHE8Ix5x81Zy8Zjqib8wCgYIKoZIzj0EAwIw\n"
	"HzEdMBsGA1UEAwwUdGlueS1lZGdlIHRlc3QgZGwgQ0EwIBcNMjYxMTI0MDIyMTU2\n"
	"WhgPMjEyNjEwMzEwMjIxNTZaMBgxFjAUBgNVBAMMDWRvd25sb2FkLnRlc3QwWTAT\n"
	"BgcqhkjOPQIBBggqhkjOPQMBBwNCAAQLtjOdNdKlSFopHgHm/eopZaQ641Orvafn\n"
	"EYqoDXr9Km2Zt2JUagEgzifdVJSpmWFqOKipQ6J1XfZUmeR8sBuLozwwOjAYBgNV\n"
	"HREEETAPgg1kb3dubG9hZC50ZXN0MAkGA1UdEwQCMAAwEwYDVR0lBAwwCgYIKwYB\n"
	"BQUHAwEwCgYIKoZIzj0EAwIDSAAwRQIgNHESCU+HirdjEqrnXYvjEcyTIYzqYjJd\n"
	"z+jGoPOatE0CIQDqxmez4+fsXPCAhyXKB6pRJdG9NaXB2+8evy0sdkyd5Q==\n"
	"-----END CERTIFICATE-----\n";

static const char srv_key[] =
	"-----BEGIN EC PRIVATE KEY-----\n"
	"MHcCAQEEIPYAplKjWxip5ko4is5C2ptThvM8loetFLuUvoUYTmpEoAoGCCqGSM49\n"
	"AwEHoUQDQgAEC7YznTXSpUhaKR4B5v3qKWWkOuNTq72n5xGKqA16/SptmbdiVGoB\n"
	"IM4n3VSUqZlhajioqUOidV32VJnkfLAbiw==\n"
	"-----END EC PRIVATE KEY-----\n";

static const char lh_cert[] =
	"-----BEGIN CERTIFICATE-----\n"
	"MIIBbjCCARWgAwIBAgIUOUNEJy73cHE8Ix5x81Zy8ZjqicAwCgYIKoZIzj0EAwIw\n"
	"HzEdMBsGA1UEAwwUdGlueS1lZGdlIHRlc3QgZGwgQ0EwIBcNMjYxMTI0MDM0MTM2\n"
	"WhgPMjEyNjEwMzEwMzQxMzZaMBQxEjAQBgNVBAMMCWxvY2FsaG9zdDBZMBMGByqG\n"
	"SM49AgEGCCqGSM49AwEHA0IABCxk4jXTofjyL5DsteHGgyJJTRc4R6prX0wQtxgI\n"
	"/PcrZ14wd/7cLpf4qZXlUG5TCKYme7ZGz0joDCIS7f9ZdoajODA2MBQGA1UdEQQN\n"
	"MAuCCWxvY2FsaG9zdDAJBgNVHRMEAjAAMBMGA1UdJQQMMAoGCCsGAQUFBwMBMAoG\n"
	"CCqGSM49BAMCA0cAMEQCIBka7nzZfLgEAMDSxws584QbxXN3NG9asZlJ6XDxSe2Q\n"
	"AiAkviF4oDkw5puD3sjKvYeupXvwCiej1TwQcQWkIbfWVA==\n"
	"-----END CERTIFICATE-----\n";

static const char lh_key[] =
	"-----BEGIN EC PRIVATE KEY-----\n"
	"MHcCAQEEIHswCK7rwrNvaxpTgQv2PvepvgHmm8F7YvKFkcB4MhuUoAoGCCqGSM49\n"
	"AwEHoUQDQgAELGTiNdOh+PIvkOy14caDIklNFzhHqmtfTBC3GAj89ytnXjB3/twu\n"
	"l/ipleVQblMIpiZ7tkbPSOgMIhLt/1l2hg==\n"
	"-----END EC PRIVATE KEY-----\n";

#endif /* TEST_TLS_CERTS_H_ */
