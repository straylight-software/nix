# straylight::nix::url

High-performance URL parsing with switchable backend (Ada/Boost.URL).

## Quick Start

```cpp
#include <straylight/nix/url/url.h>

using namespace straylight::nix::url;

// Parse a URL
auto result = parse("https://user:pass@example.com:8080/path?q=1#frag");
if (result) {
    auto& u = *result;
    std::cout << u.scheme;     // "https"
    std::cout << u.auth->host; // "example.com"
    std::cout << u.auth->port; // 8080
    std::cout << u.render_path(); // "/path"
}

// Percent encoding
auto encoded = percent_encode("hello world"); // "hello%20world"
auto decoded = percent_decode("hello%20world"); // "hello world"

// Canonicalize (remove . and ..)
auto canonical = u.canonicalize();
```

## Features

- **WHATWG + RFC 3986**: Handles differences transparently
- **Switchable backend**: Ada (WHATWG) or Boost.URL (RFC 3986)
- **Structured parsing**: Decomposed into scheme, authority, path, query, fragment
- **Percent encoding**: Full encode/decode support
- **Lenient mode**: Allows non-standard Nix flake URLs

## API Overview

| Type/Function | Description |
|---------------|-------------|
| `url` | Parsed URL structure with components |
| `authority` | Host, port, user, password |
| `parse()` | Parse URL string |
| `parse_lenient()` | Parse with Nix flake URL support |
| `percent_encode()` | Encode string for URL use |
| `percent_decode()` | Decode percent-encoded string |

## Building

```bash
buck2 build //src/straylight/nix/url:url
buck2 test //src/straylight/nix/url/tests:...
```

## See Also

- [Architecture](docs/ARCHITECTURE.md)
- [Testing](docs/TESTING.md)
