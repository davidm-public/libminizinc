% RUNS ON mzn20_fd
% RUNS ON mzn-fzn_fd
% RUNS ON mzn20_fd_linear
% RUNS ON mzn20_mip

var int: x =
	let {
		array [1..3] of var 1..10: a = [1, _, 3]
	}
	in a[2];
solve satisfy;
output ["x = ", show(x), ";\n"];
