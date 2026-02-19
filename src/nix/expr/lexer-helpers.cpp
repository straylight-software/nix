#include "lexer-helpers.h"

void nix::lexer::internal::init_loc(Parser::location_type* loc) {
  loc->beginOffset = loc->endOffset = 0;
}

void nix::lexer::internal::adjust_loc(yyscan_t yyscanner, Parser::location_type* loc, const char* s,
                                     size_t len) {
  loc->stash();

  LexerState& lexer_state = *yyget_extra(yyscanner);

  if (lexer_state.docCommentDistance == 1) {
    // Preceding token was a doc comment.
    ParserLocation doc;
    doc.beginOffset = lexer_state.lastDocCommentLoc.beginOffset;
    ParserLocation doc_end;
    doc_end.beginOffset = lexer_state.lastDocCommentLoc.endOffset;
    DocComment doc_comment{lexer_state.at(doc), lexer_state.at(doc_end)};
    pos_idx_t loc_pos = lexer_state.at(*loc);
    lexer_state.positionToDocComment.emplace(loc_pos, doc_comment);
  }
  lexer_state.docCommentDistance++;

  loc->beginOffset = loc->endOffset;
  loc->endOffset += len;
}
