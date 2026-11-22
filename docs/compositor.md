# Compositor damage and coverage

The compositor repaints invalidated screen rectangles into the framebuffer
shadow rather than maintaining a separate image for each layer. It searches
the visible window stack from the top for an opaque client. Where one
covers the damage, the client supplies the first pixels directly; the
desktop and lower windows are not copied there.

For partial coverage the compositor splits the damage into the covered
rectangle and up to four disjoint remainders. Each original damage
rectangle allows at most four splits. When that budget is spent, any
remaining piece without a fully covering client is painted from the
desktop up. Decorations and shadows above a piece's starting client
still paint in stack order. Shadows are not treated as opaque.

The cursor is painted last, once over the original damage rectangle.
Damage collection, framebuffer publication, workspace rules, and the
window ABI are unchanged.
