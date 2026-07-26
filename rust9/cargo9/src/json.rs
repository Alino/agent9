//! Minimal recursive-descent JSON reader, scoped to the crates.io sparse-index
//! shape: no numeric type is needed (every field cargo9 reads — vers, req,
//! cksum — is already a JSON string), just enough to walk objects/arrays.

#[derive(Debug, Clone)]
pub enum Value {
    Null,
    Bool(bool),
    // Not read anywhere today (every field cargo9 cares about is a JSON
    // string), but a real JSON parser must still parse PAST numeric fields
    // it doesn't use rather than erroring on them.
    #[allow(dead_code)]
    Num(f64),
    Str(String),
    Arr(Vec<Value>),
    Obj(Vec<(String, Value)>),
}

impl Value {
    pub fn get(&self, key: &str) -> Option<&Value> {
        match self {
            Value::Obj(pairs) => pairs.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Value::Str(s) => Some(s.as_str()),
            _ => None,
        }
    }

    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Value::Bool(b) => Some(*b),
            _ => None,
        }
    }

    pub fn as_array(&self) -> Option<&[Value]> {
        match self {
            Value::Arr(v) => Some(v.as_slice()),
            _ => None,
        }
    }
}

struct Parser<'a> {
    bytes: &'a [u8],
    pos: usize,
}

impl<'a> Parser<'a> {
    fn skip_ws(&mut self) {
        while self.pos < self.bytes.len() && matches!(self.bytes[self.pos], b' ' | b'\t' | b'\r' | b'\n') {
            self.pos += 1;
        }
    }

    fn peek(&self) -> Option<u8> {
        self.bytes.get(self.pos).copied()
    }

    fn expect(&mut self, b: u8) -> Result<(), String> {
        if self.peek() == Some(b) {
            self.pos += 1;
            Ok(())
        } else {
            Err(format!("json: expected '{}' at byte {}", b as char, self.pos))
        }
    }

    fn parse_value(&mut self) -> Result<Value, String> {
        self.skip_ws();
        match self.peek() {
            Some(b'"') => self.parse_string().map(Value::Str),
            Some(b'{') => self.parse_object(),
            Some(b'[') => self.parse_array(),
            Some(b't') => self.parse_lit("true", Value::Bool(true)),
            Some(b'f') => self.parse_lit("false", Value::Bool(false)),
            Some(b'n') => self.parse_lit("null", Value::Null),
            Some(c) if c == b'-' || c.is_ascii_digit() => self.parse_number(),
            _ => Err(format!("json: unexpected byte at {}", self.pos)),
        }
    }

    fn parse_lit(&mut self, lit: &str, val: Value) -> Result<Value, String> {
        let end = self.pos + lit.len();
        if end <= self.bytes.len() && &self.bytes[self.pos..end] == lit.as_bytes() {
            self.pos = end;
            Ok(val)
        } else {
            Err(format!("json: expected '{}' at byte {}", lit, self.pos))
        }
    }

    fn parse_number(&mut self) -> Result<Value, String> {
        let start = self.pos;
        if self.peek() == Some(b'-') {
            self.pos += 1;
        }
        while matches!(self.peek(), Some(c) if c.is_ascii_digit() || c == b'.' || c == b'e' || c == b'E' || c == b'+' || c == b'-') {
            self.pos += 1;
        }
        let s = std::str::from_utf8(&self.bytes[start..self.pos]).map_err(|e| e.to_string())?;
        s.parse::<f64>().map(Value::Num).map_err(|e| format!("json: bad number '{}': {}", s, e))
    }

    fn parse_string(&mut self) -> Result<String, String> {
        self.expect(b'"')?;
        let mut out = String::new();
        loop {
            match self.peek() {
                None => return Err("json: unterminated string".to_string()),
                Some(b'"') => {
                    self.pos += 1;
                    return Ok(out);
                }
                Some(b'\\') => {
                    self.pos += 1;
                    match self.peek() {
                        Some(b'"') => { out.push('"'); self.pos += 1; }
                        Some(b'\\') => { out.push('\\'); self.pos += 1; }
                        Some(b'/') => { out.push('/'); self.pos += 1; }
                        Some(b'n') => { out.push('\n'); self.pos += 1; }
                        Some(b't') => { out.push('\t'); self.pos += 1; }
                        Some(b'r') => { out.push('\r'); self.pos += 1; }
                        Some(b'b') => { out.push('\u{8}'); self.pos += 1; }
                        Some(b'f') => { out.push('\u{c}'); self.pos += 1; }
                        Some(b'u') => {
                            self.pos += 1;
                            if self.pos + 4 > self.bytes.len() {
                                return Err("json: bad \\u escape".to_string());
                            }
                            let hex = std::str::from_utf8(&self.bytes[self.pos..self.pos + 4])
                                .map_err(|e| e.to_string())?;
                            let cp = u32::from_str_radix(hex, 16)
                                .map_err(|e| format!("json: bad \\u escape '{}': {}", hex, e))?;
                            if let Some(c) = char::from_u32(cp) {
                                out.push(c);
                            }
                            self.pos += 4;
                        }
                        _ => return Err("json: bad escape".to_string()),
                    }
                }
                Some(_) => {
                    // Advance one UTF-8 codepoint at a time so multi-byte
                    // sequences in crate metadata (author names etc.) survive.
                    let rest = std::str::from_utf8(&self.bytes[self.pos..])
                        .map_err(|e| e.to_string())?;
                    let ch = rest.chars().next().ok_or("json: bad utf8")?;
                    out.push(ch);
                    self.pos += ch.len_utf8();
                }
            }
        }
    }

    fn parse_array(&mut self) -> Result<Value, String> {
        self.expect(b'[')?;
        let mut items = Vec::new();
        self.skip_ws();
        if self.peek() == Some(b']') {
            self.pos += 1;
            return Ok(Value::Arr(items));
        }
        loop {
            items.push(self.parse_value()?);
            self.skip_ws();
            match self.peek() {
                Some(b',') => { self.pos += 1; self.skip_ws(); }
                Some(b']') => { self.pos += 1; break; }
                _ => return Err(format!("json: expected ',' or ']' at byte {}", self.pos)),
            }
        }
        Ok(Value::Arr(items))
    }

    fn parse_object(&mut self) -> Result<Value, String> {
        self.expect(b'{')?;
        let mut pairs = Vec::new();
        self.skip_ws();
        if self.peek() == Some(b'}') {
            self.pos += 1;
            return Ok(Value::Obj(pairs));
        }
        loop {
            self.skip_ws();
            let key = self.parse_string()?;
            self.skip_ws();
            self.expect(b':')?;
            let val = self.parse_value()?;
            pairs.push((key, val));
            self.skip_ws();
            match self.peek() {
                Some(b',') => { self.pos += 1; }
                Some(b'}') => { self.pos += 1; break; }
                _ => return Err(format!("json: expected ',' or '}}' at byte {}", self.pos)),
            }
        }
        Ok(Value::Obj(pairs))
    }
}

/// Parse a single JSON value from a byte slice.
pub fn parse(bytes: &[u8]) -> Result<Value, String> {
    let mut p = Parser { bytes, pos: 0 };
    let v = p.parse_value()?;
    p.skip_ws();
    Ok(v)
}

/// crates.io's sparse index is newline-delimited JSON: one object per
/// published version, one line each. A malformed/blank line must not take
/// down the rest of the file.
pub fn parse_ndjson(bytes: &[u8]) -> Vec<Value> {
    let text = String::from_utf8_lossy(bytes);
    let mut out = Vec::new();
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        if let Ok(v) = parse(line.as_bytes()) {
            out.push(v);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_a_real_sparse_index_line() {
        let line = br#"{"name":"catr","vers":"0.1.2","deps":[{"name":"colored","req":"^2.0.0","features":[],"optional":false,"default_features":true,"target":null,"kind":"normal"}],"cksum":"9e13a7f4f3c2632473c8cdea79bfed3828d3ecd2c31afd806d85c29418c6dce3","features":{},"yanked":false}"#;
        let v = parse(line).unwrap();
        assert_eq!(v.get("name").and_then(Value::as_str), Some("catr"));
        assert_eq!(v.get("vers").and_then(Value::as_str), Some("0.1.2"));
        assert_eq!(v.get("yanked").and_then(Value::as_bool), Some(false));
        let deps = v.get("deps").and_then(Value::as_array).unwrap();
        assert_eq!(deps.len(), 1);
        assert_eq!(deps[0].get("name").and_then(Value::as_str), Some("colored"));
        assert_eq!(deps[0].get("optional").and_then(Value::as_bool), Some(false));
    }

    #[test]
    fn parses_escapes_and_unicode() {
        let text = "\"a\\\"b\\\\c\u{e9}\"";
        let v = parse(text.as_bytes()).unwrap();
        assert_eq!(v.as_str(), Some("a\"b\\c\u{e9}"));
    }

    #[test]
    fn ndjson_skips_blank_and_malformed_lines_without_losing_the_rest() {
        let bytes = b"{\"name\":\"a\"}\n\n{not json}\n{\"name\":\"b\"}\n";
        let vals = parse_ndjson(bytes);
        assert_eq!(vals.len(), 2);
        assert_eq!(vals[0].get("name").and_then(Value::as_str), Some("a"));
        assert_eq!(vals[1].get("name").and_then(Value::as_str), Some("b"));
    }

    #[test]
    fn nested_object_and_array() {
        let v = parse(br#"{"features":{"default":["a","b"]},"n":42}"#).unwrap();
        let feats = v.get("features").unwrap();
        let default = feats.get("default").and_then(Value::as_array).unwrap();
        assert_eq!(default.len(), 2);
        assert_eq!(default[0].as_str(), Some("a"));
    }
}
