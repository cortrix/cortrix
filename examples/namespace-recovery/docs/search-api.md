# Search service API

## Pagination

List endpoints page with `page_size` and `page_token`. `page_size` defaults to 20, with a maximum of 100. Pass back the `next_page_token` from the previous response to get the next page. Jumping to an arbitrary page is not supported.

## Timeouts

A single query times out on the server after 3 seconds and returns 504 with the partial results finished so far.
