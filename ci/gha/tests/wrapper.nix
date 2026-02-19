{
  stdenv ? "stdenv",
  withInstrumentation ? false,
}@args:
import ./. (
  args
  // {
    getStdenv = p: p.${stdenv};
    withSanitizers = withInstrumentation;
    withCoverage = withInstrumentation;
  }
)
