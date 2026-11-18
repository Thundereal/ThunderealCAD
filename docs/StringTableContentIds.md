# String ids from their content

Status: design agreed with the user 2026-11-18 ("Good. Write the doc and
start implementing"). Phase 1 (sec 8) built on branch `Worker` (sec 9). The
string table and the transaction log are also the remote session oplab's
area: every phase here is a separate commit for that review.

Companion to `docs/WorkerProcess.md` (sec 15 leased ids, sec 19 and 20,
where the problems below were found) and `docs/TransactionLogJournal.md`
(the log, which keeps the table's rows; cited as "log sec").

## 1. Why

An element's mapped name spells the ids of the strings it is built from
(`#1efaa;Wire#1efa8;:Hb42:b,W`), and names are compared as text. A string's
id is a counter of its table. So one content under two ids is two
different names for one element, and every way a table can come to give a
content a second id breaks a reference somewhere:

- a string nothing holds is freed, and made again later under a new id;
  the log keeps one content under two ids because of it (log sec 27.41 Q2,
  the table is a multiset);
- another process's table, or another document's, gives the same content
  its own id: a worker's result is refused ("string 131105 is held here as
  772", WorkerProcess sec 19-20) and read by content, every name rewritten;
- a copy of a table that differs in any way -- what it holds, its naming
  mode (WorkerProcess sec 20), how a string is split -- makes the content
  again, and every name built on it differs: Binder032's wire.

Leased id blocks (WorkerProcess sec 15) keep two processes from giving the
same id to different strings; nothing keeps them from giving one string two
ids.

## 2. The table today

`App::StringHasher`, src/App/StringHasher.{h,cpp}:

- `StringID`: a `long` id (32 bits on Windows), `_data`, `_postfix`, flags,
  and `_sids`, the strings it refers to. Spelled `#<hex>` or
  `#<hex>:<hex index>` (`StringID::toString`).
- A reference is either **structural** -- the whole prefix is another
  string (`PrefixID`, `PrefixIDIndex`, `Indexed`) or the whole postfix is
  (`PostfixEncoded`) -- or an id **spelled inside free text**, as the
  `#387` of the postfix `;:G#387;FUS;:H-4f7:b,E` (a postfix with a `#` is
  kept as text, `getID`, StringHasher.cpp:1001). Both are in `_sids`.
- Saved (`saveStream`) with embedded ids, not flattened: each row is its
  id, its flags, the ids of `_sids`, and only the text that is not a
  structural reference.
- Looked up by its bytes (`StringIDHasher`: data and postfix), ids spelled
  in them included: identity depends on the spelling of every id inside.
- One string has **two in-memory forms**: made in a session, an encoded
  postfix holds `#<id>`; read back from a stream it holds the referenced
  string's text (`restoreStreamNew`, `d._postfix = d._sids[0]._sid->_data`).
  `getID` looks both up (StringHasher.cpp:985 and :1100).

## 3. The design

### 3.1 Identity: a digest

Each string has a 128-bit **digest** of its canonical content, BLAKE2s-128
(RFC 7693, `QCryptographicHash::Blake2s_128`; Python's
`hashlib.blake2s(digest_size=16)` gives the same, which the tests use). The
canonical content (version 1) is the bytes

    "FCSID1" 0x00
    kind      1 byte: Binary (1) | Hashed (2) of the flags
    data      u32 length, then the canonical data
    postfix   u32 length, then the canonical postfix

lengths little-endian, where

- the canonical data of a string whose prefix is structural (`Indexed`,
  `PrefixID`, `PrefixIDIndex`) is 0x01 and the referenced string's digest,
  then for `PrefixIDIndex` the `:` that follows it; of a binary or hashed
  string its bytes; else its text with every id spelled inside it
  replaced (below);
- the canonical postfix of a `PostfixEncoded` string is 0x01 and the
  referenced string's digest; else its text with ids replaced;
- an id spelled inside text is `#` and hex digits naming a string in
  `_sids`: replaced by 0x01 and that string's digest. A `#` naming no
  string of `_sids` (another table's, or no id) is left as it is.

A referenced string's digest is a field of it, made when it was: a digest
is one pass over the string's own bytes, no recursion, no table lookup.
Both in-memory forms (sec 2) give the same digest; so do two tables that
gave the same content different ids. Strings a string holds that are not
in its text (`_sids` beyond its references) are history kept alive, not
identity: today's lookup ignores them too.

Measured (mini, 2026-11-18): BLAKE2s-128 of a 33-byte name 133-152 ns,
`qHash` 10-12 ns. A digest is made only for a new string: scanner.FCStd's
recompute makes 11,593 (40,783 to 52,376 strings in 13.8 s), 1.6 ms of
digests; reading its 40,783-string table, about 6 ms. The table's whole
share of that recompute, sampled, is about 10 ms.

### 3.2 The spelled id: a short prefix of it

A name spells a string by a **prefix** of its digest: 32 bits by default,
7 characters of base 32, under a marker other than `#` so that a name of
an older file reads as it did. Levels: 32, 64, 128 bits.

The marker: across 505,495 element names (scanner.FCStd and the tree's
PartDesign and example files) the only punctuation used is `# ( ) , - :
; @ |`. Of the rest, `$` begins a label in a subname, `+ - * / ^ % < > = !
&` are an expression's, `< > &` an XML attribute's: `~` is the candidate
(or `` ` ``, `{`), to be checked against the subname and expression
grammars before phase 2 fixes it.

**The registry.** The table keeps, append-only, one entry per contested
prefix: the prefix, its level, and its owner's canonical content. A string
is spelled at the first level whose prefix has no entry, or whose entry's
owner is its content. Entries are never removed: a string freed and made
again is spelled as it was, whatever else came meanwhile.

Expected entries: N^2/2 / 2^32 -- 0.2 for scanner's 40,000 strings, 116 for
a million. Almost every string is spelled at 32 bits, which is about
today's length (`#23ef5` is 6 characters) and fits today's `long`.

### 3.3 Collisions are found, never assumed away

A lookup that finds a digest, or a prefix, compares the content (canonical
form, references by digest) -- as today's lookup compares bytes after the
hash matched. So a collision at any level is found the moment the second
string comes into a table, and two contents meet only in a table: a
worker's result put in, a branch merged, a string imported from another
document. The rule is one at every level:

- in one table, the string there first keeps the spelling; the other
  moves up a level, the registry recording the owner by content;
- where tables meet, the authority's (the document's, the branch merged
  into) keeps it; the incoming string moves up, and the incoming names
  that spell it -- and those of incoming strings built on it, whose
  digests mix its own -- are rewritten as they come in: the by-content
  import (`RestoreStrings`) narrowed to that family.

A collision of full 128-bit digests (about 1.5e-27 for a million strings)
is handled the same way: the second content gets the digest of its content
and a counter, its owner recorded by content.

A worker that makes a string the client made meanwhile, with the same
32-bit prefix and another content: 5,000 new in the job, 100 here, about
1 in 10,000 jobs.

## 4. What changes, and for whom

- **Ids become `int64_t`** where they are held (`StringID::_id`, `IndexID`,
  rows, the log's columns, Python): the 32-bit level fits a `long`, the
  higher ones do not.
- **The table's order.** Saved rows go in creation order (a string after
  every string it refers to), not id order: a sequence number per string.
  The log's "rows past the highest written" (`rows(after)`, `LowTaken`,
  WorkerProcess sec 15) go by that sequence -- the log's part (oplab).
- **Workers**: no leases; a result's strings come back as they are, the
  check (`takeRows`) only asking that equal spellings be equal content.
- **Merges and imports** (`merge`, `importID`, `RestoreStrings`): identity
  across tables is the digest; a rewrite only where sec 3.3 moved a string.
- **Compaction** drops unheld strings; the ids of those kept do not change.
- **Hashed strings** (`Hashed`, the SHA-1 of a long name): the SHA-1 must
  be taken over the canonical text, or an id inside it that moved up a
  level would change the hashed string's identity.
- **The naming mode** (`IndexedNames`) stays the document's: it decides how
  a name is split, so which strings there are.

## 5. Old files

Counter ids stay valid under `#`. On open, a schema bump rewrites a file's
table to spelled digests and every name and reference with it -- the
by-content import, once (about the 3 s of the GUI's by-content put-in on
scanner; to measure). Read-only uses can keep the old ids: a process that
holds the old table finds them by content.

## 6. Python and diagnostics

`StringID.Digest` (32 hex characters); `StringHasher.duplicates()`: the
groups of ids of one table whose contents are one (same digest) -- what
sec 1 counts.

## 7. Tests

- Two tables building one content in different orders, under different
  ids: equal digests, derived strings included.
- A table saved and read back: every digest as before (both forms, sec 2).
- The digest of plain, postfixed, encoded and free-text-id strings checked
  against Python's `hashlib.blake2s` over sec 3.1's bytes: the format pinned.
- Later phases: a forced collision (a test hook shortening the prefix)
  through each rule of sec 3.3; a worker's result with no leases; an old
  file migrated, its references holding.

## 8. Phases

1. **Identity** (this commit): the digest of every string, computed where
   strings are put in (`StringHasher::insert`), the Python attribute and
   `duplicates()`, the tests. No format change, no change of behaviour.
   Measured with it: the duplicates of scanner's table, and of a worker's
   copy.
2. **Spelled digests for new strings**, behind a schema bump: `int64_t`
   ids, the marker, the registry, creation order, verified lookups.
3. **The log** by creation sequence (oplab).
4. **Workers** without leases; conflicts by sec 3.3.
5. **Old files** migrated on open.

## 9. Phase 1, as built (2026-11-18)

- `StringID::digest()` / `digestText()`, made in `StringHasher::insert()`
  -- every way a string comes into a table goes through it: `getID`, the
  two stream readers, `insertRows`, `takeRows`, `merge`, `importID`. The
  strings a string refers to are in the table before it, their digests
  made. `StringHasher::duplicates()`; Python `StringID.Digest`,
  `StringHasher.duplicates()`.
- Tests (tests/src/App/StringHasher.cpp): `digestIsPinnedToItsFormat`
  (against `hashlib.blake2s`), `oneContentOneDigestUnderOtherIds` (two
  tables, other ids, both naming modes, a name built on another by its
  spelled id), `aTableReadBackKeepsEveryDigest` (both in-memory forms),
  `duplicatesFindsOneContentUnderTwoIds` (another table's rows taken in).
- Cost: scanner.FCStd opens in 2.67 s as before, its 40,783 strings
  digested.

Measured with it, scanner.FCStd:

- No table holds one content twice: after the open, after a recompute
  here, in a worker's copy before and after its recompute -- 0 groups.
  The duplicates of sec 1 are between tables, and across time.
- Binder032 (WorkerProcess sec 20) is the second: after a worker's result
  is put in, the strings of its wire's old name -- `#1efaa`
  `Edge989;:Hb42,E`, `#1efa8` the edge pair -- are freed, and the geometry
  search that re-resolves `Pad050.Profile` makes them again as `#22b1d`
  and `#22b1b`: digests `7679bace5287...` and `279341f51669...`, those of
  the freed ones. One content, a new id, a new name for the wire. A
  recompute here keeps them held. With spelled digests (phase 2) it gets
  its old spelling back, and `Pad050`'s name still resolves.
