- `LONEJSON_WITH_OIDC` enables OAuth2/OIDC helpers. It requires `LONEJSON_WITH_CURL`, `LONEJSON_WITH_OPENSSL`, and `LONEJSON_WITH_JWT`.

- `lonejson_base64url_decoded_len`
- `lonejson_base64url_decode`
- `lonejson_jwt_parse_compact`
- short aliases `lj_base64url_decoded_len`, `lj_base64url_decode`,
  `lj_jwt_parse_compact`

- `LONEJSON_WITH_OPENSSL` enables OpenSSL-backed cryptographic internals.
- `LONEJSON_WITH_JWT` enables JWT, JWK, JWKS, and claim/signature validation
  APIs. It requires `LONEJSON_WITH_OPENSSL`.
- `LONEJSON_WITH_CURL` enables existing curl adapter APIs.
- `LONEJSON_WITH_OIDC` enables OAuth2/OIDC helpers. It requires
  `LONEJSON_WITH_CURL`, `LONEJSON_WITH_OPENSSL`, and `LONEJSON_WITH_JWT`.
