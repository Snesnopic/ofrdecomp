    2bd0: 41 56                        	pushq	%r14
    2bd2: 53                           	pushq	%rbx
    2bd3: 50                           	pushq	%rax
    2bd4: 48 89 fb                     	movq	%rdi, %rbx
    2bd7: 89 73 08                     	movl	%esi, 0x8(%rbx)
    2bda: 89 f0                        	movl	%esi, %eax
    2bdc: c1 e8 10                     	shrl	$0x10, %eax
    2bdf: 81 fe ff ff 00 00            	cmpl	$0xffff, %esi           ## imm = 0xFFFF
    2be5: 0f 97 c1                     	seta	%cl
    2be8: 0f 46 c6                     	cmovbel	%esi, %eax
    2beb: 0f b6 f9                     	movzbl	%cl, %edi
    2bee: c1 e7 04                     	shll	$0x4, %edi
    2bf1: 3d 00 01 00 00               	cmpl	$0x100, %eax            ## imm = 0x100
    2bf6: 72 06                        	jb	0x2bfe <_OptimFROG_infoFile+0x26e>
    2bf8: c1 e8 08                     	shrl	$0x8, %eax
    2bfb: 83 cf 08                     	orl	$0x8, %edi
    2bfe: 83 f8 10                     	cmpl	$0x10, %eax
    2c01: 72 06                        	jb	0x2c09 <_OptimFROG_infoFile+0x279>
    2c03: c1 e8 04                     	shrl	$0x4, %eax
    2c06: 83 c7 04                     	addl	$0x4, %edi
    2c09: 83 f8 04                     	cmpl	$0x4, %eax
    2c0c: 72 06                        	jb	0x2c14 <_OptimFROG_infoFile+0x284>
    2c0e: c1 e8 02                     	shrl	$0x2, %eax
    2c11: 83 c7 02                     	addl	$0x2, %edi
    2c14: 83 f8 01                     	cmpl	$0x1, %eax
    2c17: 0f 97 c0                     	seta	%al
    2c1a: 0f b6 c8                     	movzbl	%al, %ecx
    2c1d: 01 f9                        	addl	%edi, %ecx
    2c1f: bf 01 00 00 00               	movl	$0x1, %edi
    2c24: d3 e7                        	shll	%cl, %edi
    2c26: 39 f7                        	cmpl	%esi, %edi
    2c28: 0f 92 c1                     	setb	%cl
    2c2b: d3 e7                        	shll	%cl, %edi
    2c2d: 89 7b 14                     	movl	%edi, 0x14(%rbx)
    2c30: 89 53 10                     	movl	%edx, 0x10(%rbx)
    2c33: 48 8b 03                     	movq	(%rbx), %rax
    2c36: 48 85 c0                     	testq	%rax, %rax
    2c39: 74 0b                        	je	0x2c46 <_OptimFROG_infoFile+0x2b6>
    2c3b: 48 89 c7                     	movq	%rax, %rdi
    2c3e: e8 7d de ff ff               	callq	0xac0 <_dnAfN7>
    2c43: 8b 7b 14                     	movl	0x14(%rbx), %edi
    2c46: 48 c7 03 00 00 00 00         	movq	$0x0, (%rbx)
    2c4d: 01 ff                        	addl	%edi, %edi
    2c4f: 48 c1 e7 02                  	shlq	$0x2, %rdi
    2c53: e8 d8 dd ff ff               	callq	0xa30 <_D50BD>
    2c58: 48 89 03                     	movq	%rax, (%rbx)
    2c5b: 48 85 c0                     	testq	%rax, %rax
    2c5e: 0f 84 8f 00 00 00            	je	0x2cf3 <_OptimFROG_infoFile+0x363>
    2c64: c7 00 00 00 00 00            	movl	$0x0, (%rax)
    2c6a: 8b 53 08                     	movl	0x8(%rbx), %edx
    2c6d: 8b 4b 14                     	movl	0x14(%rbx), %ecx
    2c70: 39 ca                        	cmpl	%ecx, %edx
    2c72: 73 21                        	jae	0x2c95 <_OptimFROG_infoFile+0x305>
    2c74: 66 66 66 2e 0f 1f 84 00 00 00 00 00  	nopw	%cs:(%rax,%rax)
    2c80: 01 d1                        	addl	%edx, %ecx
    2c82: c7 04 88 00 00 00 00         	movl	$0x0, (%rax,%rcx,4)
    2c89: ff c2                        	incl	%edx
    2c8b: 8b 4b 14                     	movl	0x14(%rbx), %ecx
    2c8e: 39 ca                        	cmpl	%ecx, %edx
    2c90: 72 ee                        	jb	0x2c80 <_OptimFROG_infoFile+0x2f0>
    2c92: 8b 53 08                     	movl	0x8(%rbx), %edx
    2c95: 31 f6                        	xorl	%esi, %esi
    2c97: 85 d2                        	testl	%edx, %edx
    2c99: 74 2c                        	je	0x2cc7 <_OptimFROG_infoFile+0x337>
    2c9b: ba 01 00 00 00               	movl	$0x1, %edx
    2ca0: eb 13                        	jmp	0x2cb5 <_OptimFROG_infoFile+0x325>
    2ca2: 66 66 66 66 66 2e 0f 1f 84 00 00 00 00 00    	nopw	%cs:(%rax,%rax)
    2cb0: 8b 4b 14                     	movl	0x14(%rbx), %ecx
    2cb3: ff c2                        	incl	%edx
    2cb5: 8d 4c 11 ff                  	leal	-0x1(%rcx,%rdx), %ecx
    2cb9: c7 04 88 01 00 00 00         	movl	$0x1, (%rax,%rcx,4)
    2cc0: 8b 73 08                     	movl	0x8(%rbx), %esi
    2cc3: 39 f2                        	cmpl	%esi, %edx
    2cc5: 72 e9                        	jb	0x2cb0 <_OptimFROG_infoFile+0x320>
    2cc7: 89 73 0c                     	movl	%esi, 0xc(%rbx)
    2cca: be 01 00 00 00               	movl	$0x1, %esi
    2ccf: 48 89 df                     	movq	%rbx, %rdi
    2cd2: 48 83 c4 08                  	addq	$0x8, %rsp
    2cd6: 5b                           	popq	%rbx
    2cd7: 41 5e                        	popq	%r14
    2cd9: e9 c2 01 00 00               	jmp	0x2ea0 <_OptimFROG_infoFile+0x510>
    2cde: 48 89 c7                     	movq	%rax, %rdi
    2ce1: e8 1c 67 01 00               	callq	0x19402 <dyld_stub_binder+0x19402>
    2ce6: e8 1d 67 01 00               	callq	0x19408 <dyld_stub_binder+0x19408>
    2ceb: 48 8b 03                     	movq	(%rbx), %rax
    2cee: e9 68 ff ff ff               	jmp	0x2c5b <_OptimFROG_infoFile+0x2cb>
    2cf3: bf 28 00 00 00               	movl	$0x28, %edi
    2cf8: e8 ff 66 01 00               	callq	0x193fc <dyld_stub_binder+0x193fc>
    2cfd: 49 89 c6                     	movq	%rax, %r14
    2d00: 8b 43 14                     	movl	0x14(%rbx), %eax
    2d03: c1 e0 03                     	shll	$0x3, %eax
    2d06: 48 8d 0d c8 6b 01 00         	leaq	0x16bc8(%rip), %rcx     ## 0x198d5 <dyld_stub_binder+0x198d5>
    2d0d: 31 d2                        	xorl	%edx, %edx
    2d0f: 4c 89 f7                     	movq	%r14, %rdi
    2d12: 48 89 ce                     	movq	%rcx, %rsi
    2d15: 41 89 c0                     	movl	%eax, %r8d
    2d18: e8 53 05 00 00               	callq	0x3270 <_OptimFROG_infoFile+0x8e0>
    2d1d: 48 8d 05 7c f2 01 00         	leaq	0x1f27c(%rip), %rax     ## 0x21fa0 <dyld_stub_binder+0x21fa0>
    2d24: 48 8d 0d 75 05 00 00         	leaq	0x575(%rip), %rcx       ## 0x32a0 <_OptimFROG_infoFile+0x910>
    2d2b: 4c 89 f7                     	movq	%r14, %rdi
    2d2e: 48 89 c6                     	movq	%rax, %rsi
    2d31: 48 89 ca                     	movq	%rcx, %rdx
    2d34: e8 db 66 01 00               	callq	0x19414 <dyld_stub_binder+0x19414>
    2d39: 48 89 c3                     	movq	%rax, %rbx
    2d3c: 4c 89 f7                     	movq	%r14, %rdi
    2d3f: e8 ca 66 01 00               	callq	0x1940e <dyld_stub_binder+0x1940e>
    2d44: 48 89 df                     	movq	%rbx, %rdi
    2d47: e8 ce 66 01 00               	callq	0x1941a <dyld_stub_binder+0x1941a>
    2d4c: 0f 1f 40 00                  	nopl	(%rax)
    2d50: 31 d2                        	xorl	%edx, %edx
    2d52: 83 7f 08 00                  	cmpl	$0x0, 0x8(%rdi)
    2d56: 74 1d                        	je	0x2d75 <_OptimFROG_infoFile+0x3e5>
    2d58: 48 8b 07                     	movq	(%rdi), %rax
    2d5b: 31 c9                        	xorl	%ecx, %ecx
    2d5d: 0f 1f 00                     	nopl	(%rax)
    2d60: 8b 57 14                     	movl	0x14(%rdi), %edx
