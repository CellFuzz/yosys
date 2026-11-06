autoidx 1
attribute \top 1
module \top
  wire width 8 input 1 \a
  wire width 8 input 2 \b
  wire width 8 input 3 \c
  wire width 8 output 4 \x
  wire width 8 output 5 \y
  cell $fa \fa
    parameter \WIDTH 8
    connect \A \a
    connect \B \b
    connect \C \c
    connect \X \x
    connect \Y \y
  end
end
