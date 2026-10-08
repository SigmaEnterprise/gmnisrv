tatic bool urlchar_needs_escaping(int c)
{
    /* Escape anything that isn't an unreserved character or a
       reserved character that is legal unescaped in a path. */
    if (Curl_isunreserved((unsigned char)c)) {
        return false;
    }
    switch (c) {
    case '/': case ':': case '?': case '#':
    case '[': case ']': case '@': case '!':
    case '$': case '&': case '\'': case '(':
    case ')': case '*': case '+': case ',':
    case ';': case '=':
        return false;
    default:
        return true;
    }
}
