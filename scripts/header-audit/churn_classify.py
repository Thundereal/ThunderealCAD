"""Classify what past edits to a header actually changed, to tell which edits a pimpl, an
out-of-line body or a moved declaration would have kept out of the header.

  python churn_classify.py <repo> <since> <header> [<header> ...]

For each non-merge commit since <since> that touched the header, every changed line is put
in one bucket by where it sits in the NEW file (removed lines take the position they had):
  comment   blank or comment text only
  include   #include / preprocessor
  private   a private member of a class (data or function declaration)
  protected a protected member
  body      inside a function body (an inline definition in the header)
  public    a public member declaration, or a namespace-scope declaration
  py        a static PyObject* wrapper / PyMethodDef (only the .cpp's method table needs it)
  signal    a fastsignals::signal member
The commit's class is the most exposed bucket it has (public > protected > signal > py >
private > body > include > comment). "avoidable" = commits whose lines are all
py/private/body/include/comment: with file-local Python wrappers, a pimpl and out-of-line
bodies those would not have touched the header.
"""
import collections
import re
import subprocess
import sys


def git(repo, *args):
    return subprocess.run(["git", "-C", repo] + list(args), capture_output=True, text=True,
                          encoding="utf-8", errors="replace").stdout


class Scanner:
    def __init__(self):
        self.stack = []  # ('ns'|'class'|'body'|'enum'|'init', access)
        self.in_block_comment = False
        self.pending = ""

    def context(self):
        for k, acc in reversed(self.stack):
            if k == "body" or k == "init":
                return "body"
            if k == "class":
                return acc
            if k == "enum":
                return "public"
        return "public"

    def feed(self, line):
        s = line
        # strip comments and strings for structure
        out = []
        i = 0
        while i < len(s):
            if self.in_block_comment:
                j = s.find("*/", i)
                if j < 0:
                    i = len(s)
                    break
                self.in_block_comment = False
                i = j + 2
                continue
            if s.startswith("//", i):
                break
            if s.startswith("/*", i):
                self.in_block_comment = True
                i += 2
                continue
            c = s[i]
            if c in "\"'":
                j = i + 1
                while j < len(s) and s[j] != c:
                    j += 2 if s[j] == "\\" else 1
                i = j + 1
                out.append(" ")
                continue
            out.append(c)
            i += 1
        code = "".join(out)
        st = code.strip()
        if st.startswith("#"):
            return
        m = re.match(r"\s*(public|private|protected)\s*:", code)
        if m and self.stack and self.stack[-1][0] == "class":
            self.stack[-1] = ("class", m.group(1))
        for ch in code:
            if ch == "{":
                head = " ".join(self.pending.split())
                if self.stack and self.stack[-1][0] in ("body", "init"):
                    self.stack.append(("body", None))
                elif re.search(r"\bnamespace\b[\w\s:]*$", head) or re.search(r'extern\s*$', head):
                    self.stack.append(("ns", None))
                elif re.search(r"\benum\b", head):
                    self.stack.append(("enum", None))
                elif re.search(r"\b(class|struct|union)\s+(\w+\s+)*\w+\s*(final\s*)?(:[^;()]*)?$", head) and "(" not in head.split("class")[0].split("struct")[0]:
                    acc = "private" if re.search(r"\bclass\b", head) else "public"
                    self.stack.append(("class", acc))
                elif head.endswith(")") or re.search(r"\)\s*(const|override|final|noexcept|->.*)?\s*$", head) or head.endswith("try"):
                    self.stack.append(("body", None))
                else:
                    self.stack.append(("init", None))
                self.pending = ""
            elif ch == "}":
                if self.stack:
                    self.stack.pop()
                self.pending = ""
            elif ch == ";":
                self.pending = ""
            else:
                self.pending += ch
        self.pending = (self.pending + " ")[-400:]


def classify_commit(repo, c, path):
    diff = git(repo, "show", "--format=", "-U100000", c, "--", path)
    lines = diff.split("\n")
    try:
        start = next(i for i, l in enumerate(lines) if l.startswith("@@"))
    except StopIteration:
        return None
    sc = Scanner()
    buckets = collections.Counter()
    in_comment_new = False
    for l in lines[start + 1:]:
        if not l:
            continue
        tag, text = l[0], l[1:]
        if tag == "\\":
            continue
        ctx = sc.context()
        if tag in "+-":
            st = text.strip()
            was_in_comment = sc.in_block_comment
            if not st or st.startswith("//") or st.startswith("*") or st.startswith("/*") or was_in_comment:
                b = "comment"
            elif st.startswith("#"):
                b = "include"
            else:
                b = ctx if ctx in ("private", "protected", "public", "body") else "public"
                if b in ("public", "protected", "private") and re.search(r"static\s+PyObject\s*\*\s*\w+\s*\(|PyMethodDef", st):
                    b = "py"
                elif b in ("public", "protected") and re.search(r"signal\s*<", st):
                    b = "signal"
                if re.match(r"(public|private|protected)\s*:\s*$", st):
                    b = "include"  # a bare access label: neutral
            buckets[b] += 1
        if tag in " +":
            sc.feed(text)
    if not buckets:
        return None
    for k in ("public", "protected", "private", "body", "include", "comment"):
        if buckets.get(k):
            worst = k
            break
    order = ["public", "protected", "signal", "py", "private", "body", "include", "comment"]
    worst = next(k for k in order if buckets.get(k))
    return worst, buckets


def main():
    repo, since = sys.argv[1], sys.argv[2]
    for h in sys.argv[3:]:
        commits = git(repo, "log", "--since=" + since, "--no-merges", "--format=%h", "HEAD", "--", h).split()
        cls = collections.Counter()
        lines = collections.Counter()
        for c in commits:
            r = classify_commit(repo, c, h)
            if not r:
                continue
            cls[r[0]] += 1
            lines.update(r[1])
        n = sum(cls.values())
        avoid = n - cls["public"] - cls["protected"] - cls["signal"]
        print(f"{h}: {n} commits; most-exposed change per commit: "
              + ", ".join(f"{k} {cls[k]}" for k in ("public", "protected", "signal", "py", "private", "body", "include", "comment") if cls[k])
              + f" | avoidable with pimpl/out-of-line: {avoid} ({100*avoid/max(1,n):.0f}%)"
              + " | lines: " + ", ".join(f"{k} {v}" for k, v in lines.most_common()))


if __name__ == "__main__":
    main()
