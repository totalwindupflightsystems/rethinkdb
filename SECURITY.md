# Security Policy

## Supported Versions

RethinkDB 2.4.x and later receive security updates. Earlier versions are end-of-life.

## Reporting a Vulnerability

Please report security vulnerabilities privately to the maintainers. Do not open public issues.

- **This fork (totalwindupflightsystems/rethinkdb):** report via the GitHub Security Advisory flow — <https://github.com/totalwindupflightsystems/rethinkdb/security/advisories/new> — so fork-specific code (time-series, CDC, vector/FTS, partitioning, generated columns) is triaged by the fork maintainers.
- **Upstream RethinkDB:** email security@rethinkdb.com for issues that affect the upstream project.

We aim to acknowledge reports within 48 hours and provide an initial assessment within 5 business days.

## Security Features

- TLS encryption for driver, intracluster, and web UI connections
- User authentication and permissions system
- Certificate verification for TLS connections
- SASL-based authentication (SCRAM-SHA-256)
- Configurable network binding (localhost by default)

## Best Practices

- Always enable TLS in production deployments
- Use strong passwords and rotate credentials regularly
- Bind to localhost or trusted networks only
- Keep your RethinkDB installation up to date
- Review the administration guide for security hardening recommendations
