We are building a single pass sort algorith, which will be used to sort multiple relatively small sorts, without any barriers. 

Please plan before starting.

Build this using:
- C++ / win32 / DX12
- use vs2022, which should be available in the command prompt. make a sln/vcxproj


Context: The use case for the sort we build is
- An engine needs to sort gpu draw calls
- There are multiple sorts, lets say 20 each frame
- Each sort is between 0 and 8192 elements. 
	- Never more
	- Some will be empty
	- Few will be larger than 2k elements
- sort key is 16bit with a 16bit payload
- Assume Shader Model 6.6


The end goal is to make a sort that is as fast as possible on both low end and high end cards.
The machine this is running on has both a 2060 and a 5080 installed.


Task 1:
Build a dx12 test framework:
The test frame work should:
- Prompt its about to run (so I can stop my gpu work, I'm using the machine in parallel)
- Keep something visible while the test is running (if you decide to not have a window)
- Repeat this test many times (say 1000?), and for each hardware gpu installed
	1) generate random data to be sorted
	2) run some fake work that tries to clear the generated data from the gpus caches
	3) Run the sort
	4) verify elements are sorted
- print timing results (obviously, -only- time the actual sorting part of the gpu code)

- The test framework should also test different workload sizes (IE mostly empty, mostly small, a mix, mostly large).
	- Generate The sizes to test, and keep run the test for each of the workload sizes
- Make sure its all deterministic, so its the same thing being tested/

The test framework should be used in step 2 when optimizing the sorting algorithm

Task 2:
Write Sorting algorithms, and optimize them.

Given there are -no- sorts above 8k, my suggested starting point is
- Split into two or three dispatches
Each handles a sort of differing size
	- <64 or <128 (ThreadGroupSize 64/128)
	- <= 512  (ThreadGroupSize 512)
	- > 512 (ThreadGroupSize 1024)

I suggest:
 - You do exactly -one- dispatch indirect per group/shader. The overhead of each dispatchindirect can easily end up dominating
 	- Which is why I mention 2 or 3. I'm guessing depending on the sort workloads, the smallest batch is not worth it
 - Consider Rank sort for the <=512 elements. 
 - I think for 8k and 32 bit values, we can have it all in LDS, so lets ignore the case of spilling to UAV sort.
 - I'm guessing Bitonic or Rank sort will work as well for the larger sort, but will let you decide.
 - Feel free to be inspired by Cuda CUB

When you're iterating on optimizing the code:
- Make a subfolder for each pass `_test/pass0`
	- Save the shader code there
	- Save the timing text there
	- Save a note of what you did that pass
- commit and push everything for each iteration. you don't have to ask permission to do this.

I've put the notes from the chat we had about sorting in gpu-sorting-notes.md