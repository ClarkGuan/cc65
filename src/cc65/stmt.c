/*
 * stmt.c
 *
 * Ullrich von Bassewitz, 06.08.1998
 *
 * Original by John R. Dunning - see copyleft.jrd
 */



#include <stdio.h>
#include <string.h>

/* common */
#include "coll.h"
#include "xmalloc.h"

/* cc65 */
#include "asmcode.h"
#include "asmlabel.h"
#include "codegen.h"
#include "datatype.h"
#include "error.h"
#include "expr.h"
#include "function.h"
#include "global.h"
#include "goto.h"
#include "litpool.h"
#include "locals.h"
#include "loop.h"
#include "pragma.h"
#include "scanner.h"
#include "symtab.h"
#include "stmt.h"



/*****************************************************************************/
/*	  	  	       	     Data		     		     */
/*****************************************************************************/



/* Maximum count of cases */
#define CASE_MAX 	257



/*****************************************************************************/
/*			       Helper functions                              */
/*****************************************************************************/



static void CheckTok (token_t Tok, const char* Msg, int* PendingToken)
/* Helper function for Statement. Will check for Tok and print Msg if not
 * found. If PendingToken is NULL, it will the skip the token, otherwise
 * it will store one to PendingToken.
 */
{
    if (CurTok.Tok != Tok) {
	Error (Msg);
    } else if (PendingToken) {
	*PendingToken = 1;
    } else {
	NextToken ();
    }
}



static void CheckSemi (int* PendingToken)
/* Helper function for Statement. Will call CheckTok with the parameters
 * for a semicolon.
 */
{
    CheckTok (TOK_SEMI, "`;' expected", PendingToken);
}



static void SkipPending (int PendingToken)
/* Skip the pending token if we have one */
{
    if (PendingToken) {
	NextToken ();
    }
}



/*****************************************************************************/
/*	  	  		     Code		     		     */
/*****************************************************************************/



static int IfStatement (void)
/* Handle an 'if' statement */
{
    unsigned Label1;
    int GotBreak;

    /* Skip the if */
    NextToken ();

    /* Generate a jump label and parse the condition */
    Label1 = GetLocalLabel ();
    test (Label1, 0);

    /* Parse the if body */
    GotBreak = Statement (0);

    /* Else clause present? */
    if (CurTok.Tok != TOK_ELSE) {

      	g_defcodelabel (Label1);

     	/* Since there's no else clause, we're not sure, if the a break
     	 * statement is really executed.
     	 */
      	return 0;

    } else {

	/* Generate a jump around the else branch */
     	unsigned Label2 = GetLocalLabel ();
	g_jump (Label2);

	/* Skip the else */
     	NextToken ();

	/* Define the target for the first test */
	g_defcodelabel (Label1);

	/* Total break only if both branches had a break. */
     	GotBreak &= Statement (0);

     	/* Generate the label for the else clause */
	g_defcodelabel (Label2);

     	/* Done */
     	return GotBreak;
    }
}



static void DoStatement (void)
/* Handle the 'do' statement */
{
    /* Get the loop control labels */
    unsigned loop = GetLocalLabel ();
    unsigned lab = GetLocalLabel ();

    /* Skip the while token */
    NextToken ();

    /* Add the loop to the loop stack */
    AddLoop (oursp, loop, lab, 0, 0);

    /* Define the head label */
    g_defcodelabel (loop);

    /* Parse the loop body */
    Statement (0);

    /* Parse the end condition */
    Consume (TOK_WHILE, "`while' expected");
    test (loop, 1);
    ConsumeSemi ();

    /* Define the break label */
    g_defcodelabel (lab);

    /* Remove the loop from the loop stack */
    DelLoop ();
}



static void WhileStatement (void)
/* Handle the 'while' statement */
{
    int PendingToken;

    /* Get the loop control labels */
    unsigned loop = GetLocalLabel ();
    unsigned lab = GetLocalLabel ();

    /* Skip the while token */
    NextToken ();

    /* Add the loop to the loop stack */
    AddLoop (oursp, loop, lab, 0, 0);

    /* Define the head label */
    g_defcodelabel (loop);

    /* Test the loop condition */
    test (lab, 0);

    /* Loop body */
    Statement (&PendingToken);

    /* Jump back to loop top */
    g_jump (loop);

    /* Exit label */
    g_defcodelabel (lab);

    /* Eat remaining tokens that were delay because of line info correctness */
    SkipPending (PendingToken);

    /* Remove the loop from the loop stack */
    DelLoop ();
}



static void ReturnStatement (void)
/* Handle the 'return' statement */
{
    ExprDesc lval;

    NextToken ();
    if (CurTok.Tok != TOK_SEMI) {
       	if (HasVoidReturn (CurrentFunc)) {
       	    Error ("Returning a value in function with return type void");
       	}

	/* Evaluate the return expression. Result will be in primary */
	expression (&lval);

    	/* Convert the return value to the type of the function result */
    	if (!HasVoidReturn (CurrentFunc)) {
       	    assignadjust (GetReturnType (CurrentFunc), &lval);
    	}
    } else if (!HasVoidReturn (CurrentFunc)) {
    	Error ("Function `%s' must return a value", GetFuncName (CurrentFunc));
    }

    /* Cleanup the stack in case we're inside a block with locals */
    g_space (oursp - GetTopLevelSP (CurrentFunc));

    /* Output a jump to the function exit code */
    g_jump (GetRetLab (CurrentFunc));
}



static void BreakStatement (void)
/* Handle the 'break' statement */
{
    LoopDesc* L;

    /* Skip the break */
    NextToken ();

    /* Get the current loop descriptor */
    L = CurrentLoop ();

    /* Check if we are inside a loop */
    if (L == 0) {
	/* Error: No current loop */
	Error ("`break' statement not within loop or switch");
	return;
    }

    /* Correct the stack pointer if needed */
    g_space (oursp - L->StackPtr);

    /* Jump to the exit label of the loop */
    g_jump (L->Label);
}



static void ContinueStatement (void)
/* Handle the 'continue' statement */
{
    LoopDesc* L;

    /* Skip the continue */
    NextToken ();

    /* Get the current loop descriptor */
    L = CurrentLoop ();
    if (L) {
	/* Search for the correct loop */
	do {
	    if (L->Loop) {
		break;
	    }
	    L = L->Next;
	} while (L);
    }

    /* Did we find it? */
    if (L == 0) {
	Error ("`continue' statement not within a loop");
	return;
    }

    /* Correct the stackpointer if needed */
    g_space (oursp - L->StackPtr);

    /* Output the loop code */
    if (L->linc) {
       	g_jump (L->linc);
    } else {
       	g_jump (L->Loop);
    }
}



static void CascadeSwitch (ExprDesc* Expr)
/* Handle a switch statement for chars with a cmp cascade for the selector */
{
    unsigned ExitLab;  	     	/* Exit label */
    unsigned NextLab;  	     	/* Next case label */
    unsigned CodeLab;		/* Label that starts the actual selector code */
    int HaveBreak;   		/* Remember if we exited with break */
    int HaveDefault;		/* Remember if we had a default label */
    int lcount;	       	       	/* Label count */
    unsigned Flags;    		/* Code generator flags */
    ExprDesc lval;		/* Case label expression */
    long Val;	       		/* Case label value */


    /* Create a loop so we may break out, init labels */
    ExitLab = GetLocalLabel ();
    AddLoop (oursp, 0, ExitLab, 0, 0);

    /* Setup some variables needed in the loop  below */
    Flags = TypeOf (Expr->Type) | CF_CONST | CF_FORCECHAR;
    CodeLab = NextLab = 0;
    HaveBreak = 1;
    HaveDefault = 0;

    /* Parse the labels */
    lcount = 0;
    while (CurTok.Tok != TOK_RCURLY) {

	if (CurTok.Tok == TOK_CASE || CurTok.Tok == TOK_DEFAULT) {

	    /* If the code for the previous selector did not end with a
	     * break statement, we must jump over the next selector test.
	     */
	    if (!HaveBreak) {
		/* Define a label for the code */
		if (CodeLab == 0) {
		    CodeLab = GetLocalLabel ();
		}
		g_jump (CodeLab);
	    }

	    /* If we have a cascade label, emit it */
	    if (NextLab) {
		g_defcodelabel (NextLab);
		NextLab = 0;
	    }

	    while (CurTok.Tok == TOK_CASE || CurTok.Tok == TOK_DEFAULT) {

		/* Parse the selector */
		if (CurTok.Tok == TOK_CASE) {

		    /* Count labels */
		    ++lcount;

		    /* Skip the "case" token */
		    NextToken ();

		    /* Read the selector expression */
		    constexpr (&lval);
		    if (!IsClassInt (lval.Type)) {
			Error ("Switch quantity not an integer");
		    }

		    /* Check the range of the expression */
		    Val = lval.ConstVal;
		    switch (*Expr->Type) {

			case T_SCHAR:
			    /* Signed char */
			    if (Val < -128 || Val > 127) {
		     	  	Error ("Range error");
		     	    }
		     	    break;

		     	case T_UCHAR:
		     	    if (Val < 0 || Val > 255) {
		     	  	Error ("Range error");
		     	    }
		     	    break;

		     	case T_INT:
		     	    if (Val < -32768 || Val > 32767) {
		     	  	Error ("Range error");
		     	    }
		     	    break;

		     	case T_UINT:
		     	    if (Val < 0 || Val > 65535) {
		     	  	Error ("Range error");
		     	    }
		     	    break;

		     	default:
		     	    Internal ("Invalid type: %02X", *Expr->Type & 0xFF);
		    }

		    /* Emit a compare */
		    g_cmp (Flags, Val);

	    	    /* If another case follows after the colon (which is 
		     * currently pending and cannot be skipped since otherwise
		     * the debug infos will get wrong), we will jump to the 
		     * code if the condition is true.
	    	     */
       	    	    if (NextTok.Tok == TOK_CASE) {
		     	/* Create a code label if needed */
		     	if (CodeLab == 0) {
		     	    CodeLab = GetLocalLabel ();
		     	}
		     	g_falsejump (CF_NONE, CodeLab);
		    } else if (NextTok.Tok != TOK_DEFAULT) {
		  	/* No case follows, jump to next selector */
		  	if (NextLab == 0) {
		  	    NextLab = GetLocalLabel ();
		  	}
		     	g_truejump (CF_NONE, NextLab);
		    }

		    /* Skip the colon */
		    ConsumeColon ();

		} else {

		    /* Default case */
		    NextToken ();

		    /* Handle the pathologic case: DEFAULT followed by CASE */
		    if (CurTok.Tok == TOK_CASE) {
		  	if (CodeLab == 0) {
		   	    CodeLab = GetLocalLabel ();
		  	}
		    	g_jump (CodeLab);
		    }

		    /* Skip the colon */
		    ConsumeColon ();

		    /* Remember that we had a default label */
		    HaveDefault = 1;
		}

	    }

        }

	/* Emit a code label if we have one */
	if (CodeLab) {
	    g_defcodelabel (CodeLab);
	    CodeLab = 0;
	}

	/* Parse statements */
	if (CurTok.Tok != TOK_RCURLY) {
       	    HaveBreak = Statement (0);
	}
    }

    /* Check if we have any labels */
    if (lcount == 0 && !HaveDefault) {
     	Warning ("No case labels");
    }

    /* Define the exit label and, if there's a next label left, create this
     * one, too.
     */
    if (NextLab) {
	g_defcodelabel (NextLab);
    }
    g_defcodelabel (ExitLab);

    /* Eat the closing curly brace */
    NextToken ();

    /* End the loop */
    DelLoop ();
}



static void TableSwitch (ExprDesc* Expr)
/* Handle a switch statement via table based selector */
{
    /* Entry for one case in a switch statement */
    typedef struct {
    	long     Value;	        /* selector value */
       	unsigned Label; 	/* label for this selector */
    } SwitchEntry;

    unsigned DefaultLabel;      /* Label for default case */
    unsigned ExitLabel;	   	/* exit label */
    int lcase; 	       	       	/* label for compares */
    int HaveBreak;     	     	/* Last statement has a break */
    unsigned Flags;    	     	/* Code generator flags */
    ExprDesc lval;     	     	/* Case label expression */
    unsigned I;
    SwitchEntry* P;
    Collection SwitchTab;

    /* Initialize the collection for the switch entries */
    InitCollection (&SwitchTab);

    /* Create a look so we may break out, init labels */
    HaveBreak    = 0;     		/* Keep gcc silent */
    DefaultLabel = 0;  	       	        /* No default case until now */
    ExitLabel    = GetLocalLabel ();	/* get exit */
    AddLoop (oursp, 0, ExitLabel, 0, 0);

    /* Jump behind the code for the CASE labels */
    g_jump (lcase = GetLocalLabel ());
    while (CurTok.Tok != TOK_RCURLY) {
    	if (CurTok.Tok == TOK_CASE || CurTok.Tok == TOK_DEFAULT) {
    	    do {
    	    	if (CurTok.Tok == TOK_CASE) {
       	    	    NextToken ();
    	    	    constexpr (&lval);
	    	    if (!IsClassInt (lval.Type)) {
	    	     	Error ("Switch quantity not an integer");
	      	    }
		    P = xmalloc (sizeof (SwitchEntry));
     	    	    P->Value = lval.ConstVal;
    	    	    P->Label = GetLocalLabel ();
		    CollAppend (&SwitchTab, P);
		    g_defcodelabel (P->Label);
    	    	} else if (DefaultLabel == 0) {
    	    	    NextToken ();
     	    	    DefaultLabel = GetLocalLabel ();
		    g_defcodelabel (DefaultLabel);
    	    	} else {
		    /* We already had a default label */
		    Error ("Multiple default labels in one switch");
		    /* Try to recover */
		    NextToken ();
		}
    	    	ConsumeColon ();
    	    } while (CurTok.Tok == TOK_CASE || CurTok.Tok == TOK_DEFAULT);
	    HaveBreak = 0;
    	}
    	if (CurTok.Tok != TOK_RCURLY) {
    	    HaveBreak = Statement (0);
    	}
    }

    /* Check if we have any labels */
    if (CollCount(&SwitchTab) == 0 && DefaultLabel == 0) {
     	Warning ("No case labels");
    }

    /* Eat the closing curly brace */
    NextToken ();

    /* If the last statement doesn't have a break or return, add one */
    if (!HaveBreak) {
        g_jump (ExitLabel);
    }

    /* Actual selector code goes here */
    g_defcodelabel (lcase);

    /* Create the call to the switch subroutine */
    Flags = TypeOf (Expr->Type);
    g_switch (Flags);

    /* First entry is negative of label count */
    g_defdata (CF_INT | CF_CONST, -((int)CollCount(&SwitchTab))-1, 0);

    /* Create the case selector table */
    for (I = 0; I < CollCount (&SwitchTab); ++I) {
	P = CollAt (&SwitchTab, I);
       	g_case (Flags, P->Label, P->Value);	/* Create one label */
    }

    if (DefaultLabel != 0) {
       	g_jump (DefaultLabel);
    }
    g_defcodelabel (ExitLabel);
    DelLoop ();

    /* Free the allocated space for the labels */
    for (I = 0; I < CollCount (&SwitchTab); ++I) {
	xfree (CollAt (&SwitchTab, I));
    }

    /* Free the collection itself */
    DoneCollection (&SwitchTab);
}



static void SwitchStatement (void)
/* Handle a 'switch' statement */
{
    ExprDesc Expr;	       	/* Switch statement expression */

    /* Eat the "switch" */
    NextToken ();

    /* Read the switch expression */
    ConsumeLParen ();
    intexpr (&Expr);
    ConsumeRParen ();

    /* result of expr is in P */
    ConsumeLCurly ();

    /* Now decide which sort of switch we will create: */
    if (IsTypeChar (Expr.Type) || (CodeSizeFactor >= 200 && IsClassInt (Expr.Type))) {
       	CascadeSwitch (&Expr);
    } else {
      	TableSwitch (&Expr);
    }
}



static void ForStatement (void)
/* Handle a 'for' statement */
{
    ExprDesc lval1;
    ExprDesc lval2;
    ExprDesc lval3;
    int HaveIncExpr;
    CodeMark IncExprStart;
    CodeMark IncExprEnd;
    int PendingToken;

    /* Get several local labels needed later */
    unsigned TestLabel = GetLocalLabel ();
    unsigned lab       = GetLocalLabel ();
    unsigned IncLabel  = GetLocalLabel ();
    unsigned lstat     = GetLocalLabel ();

    /* Skip the FOR token */
    NextToken ();

    /* Add the loop to the loop stack */
    AddLoop (oursp, TestLabel, lab, IncLabel, lstat);

    /* Skip the opening paren */
    ConsumeLParen ();

    /* Parse the initializer expression */
    if (CurTok.Tok != TOK_SEMI) {
	expression (&lval1);
    }
    ConsumeSemi ();

    /* Label for the test expressions */
    g_defcodelabel (TestLabel);

    /* Parse the test expression */
    if (CurTok.Tok != TOK_SEMI) {
    	boolexpr (&lval2);
    	g_truejump (CF_NONE, lstat);
    	g_jump (lab);
    } else {
    	g_jump (lstat);
    }
    ConsumeSemi ();

    /* Remember the start of the increment expression */
    IncExprStart = GetCodePos();

    /* Label for the increment expression */
    g_defcodelabel (IncLabel);

    /* Parse the increment expression */
    HaveIncExpr = (CurTok.Tok != TOK_RPAREN);
    if (HaveIncExpr) {
    	expression (&lval3);
    }

    /* Jump to the test */
    g_jump (TestLabel);

    /* Remember the end of the increment expression */
    IncExprEnd = GetCodePos();

    /* Skip the closing paren */
    ConsumeRParen ();

    /* Loop body */
    g_defcodelabel (lstat);
    Statement (&PendingToken);

    /* If we had an increment expression, move the code to the bottom of
     * the loop. In this case we don't need to jump there at the end of
     * the loop body.
     */
    if (HaveIncExpr) {
	MoveCode (IncExprStart, IncExprEnd, GetCodePos());
    } else {
	/* Jump back to the increment expression */
	g_jump (IncLabel);
    }

    /* Skip a pending token if we have one */
    SkipPending (PendingToken);

    /* Declare the break label */
    g_defcodelabel (lab);

    /* Remove the loop from the loop stack */
    DelLoop ();
}



static int CompoundStatement (void)
/* Compound statement. Allow any number of statements inside braces. The
 * function returns true if the last statement was a break or return.
 */
{
    int GotBreak;

    /* Remember the stack at block entry */
    int OldStack = oursp;

    /* Enter a new lexical level */
    EnterBlockLevel ();

    /* Parse local variable declarations if any */
    DeclareLocals ();

    /* Now process statements in this block */
    GotBreak = 0;
    while (CurTok.Tok != TOK_RCURLY) {
     	if (CurTok.Tok != TOK_CEOF) {
     	    GotBreak = Statement (0);
     	} else {
     	    break;
     	}
    }

    /* Clean up the stack. */
    if (!GotBreak) {
	g_space (oursp - OldStack);
    }
    oursp = OldStack;

    /* Emit references to imports/exports for this block */
    EmitExternals ();

    /* Leave the lexical level */
    LeaveBlockLevel ();

    return GotBreak;
}



int Statement (int* PendingToken)
/* Statement parser. Returns 1 if the statement does a return/break, returns
 * 0 otherwise. If the PendingToken pointer is not NULL, the function will
 * not skip the terminating token of the statement (closing brace or
 * semicolon), but store true if there is a pending token, and false if there
 * is none. The token is always checked, so there is no need for the caller to
 * check this token, it must be skipped, however. If the argument pointer is
 * NULL, the function will skip the token.
 */
{
    ExprDesc lval;
    int GotBreak;

    /* Assume no pending token */
    if (PendingToken) {
	*PendingToken = 0;
    }

    /* Check for a label */
    if (CurTok.Tok == TOK_IDENT && NextTok.Tok == TOK_COLON) {

	/* Special handling for a label */
	DoLabel ();

    } else {

     	switch (CurTok.Tok) {

     	    case TOK_LCURLY:
	        NextToken ();
		GotBreak = CompoundStatement ();
	        CheckTok (TOK_RCURLY, "`{' expected", PendingToken);
	        return GotBreak;

     	    case TOK_IF:
     	    	return IfStatement ();

     	    case TOK_WHILE:
     	    	WhileStatement ();
	    	break;

	    case TOK_DO:
	    	DoStatement ();
	    	break;

	    case TOK_SWITCH:
	    	SwitchStatement ();
	    	break;

	    case TOK_RETURN:
	    	ReturnStatement ();
	    	CheckSemi (PendingToken);
	    	return 1;

	    case TOK_BREAK:
		BreakStatement ();
	    	CheckSemi (PendingToken);
		return 1;

	    case TOK_CONTINUE:
		ContinueStatement ();
	    	CheckSemi (PendingToken);
		return 1;

	    case TOK_FOR:
		ForStatement ();
		break;

	    case TOK_GOTO:
		GotoStatement ();
	    	CheckSemi (PendingToken);
		return 1;

	    case TOK_SEMI:
		/* Ignore it */
		NextToken ();
		break;

	    case TOK_PRAGMA:
		DoPragma ();
		break;

	    default:
	        /* Actual statement */
		expression (&lval);
	    	CheckSemi (PendingToken);
	}
    }
    return 0;
}



