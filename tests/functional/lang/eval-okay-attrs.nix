rec {
  as = {
    x = 123;
    y = 456;
  }
  // {
    z = 789;
  }
  // {
    z = 987;
  };
  body =
    as.a or (assert as ? z;
      as.z);
}.body
