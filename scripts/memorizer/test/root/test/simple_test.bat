@test "is 3+4=7?" {
	a=3
	b=4
	c=$((a+b))
	[[ $c == 7 ]]
}
