rec {
  x =
    {gcc}:
    {
      inherit gcc;
    };
  body = {
    inherit gcc;
  }.gcc;
}.body
