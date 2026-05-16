%{
#include "ast.h"
#include "yacc.tab.h"
#include <iostream>
#include <memory>

int yylex(YYSTYPE *yylval, YYLTYPE *yylloc);

void yyerror(YYLTYPE *locp, const char* s) {
    std::cerr << "Parser Error at line " << locp->first_line << " column " << locp->first_column << ": " << s << std::endl;
}

using namespace ast;
%}

// request a pure (reentrant) parser
%define api.pure full
// enable location in error handler
%locations
// enable verbose syntax error message
%define parse.error verbose

// keywords
%token SHOW TABLES CREATE TABLE DROP DESC INSERT INTO VALUES DELETE FROM ASC ORDER BY AS
WHERE UPDATE SET SELECT INT CHAR FLOAT INDEX AND IN OR JOIN EXIT HELP TXN_BEGIN TXN_COMMIT TXN_ABORT TXN_ROLLBACK ORDER_BY ENABLE_NESTLOOP ENABLE_SORTMERGE
%token GROUP HAVING COUNT SUM AVG MIN MAX
// non-keywords
%token LEQ NEQ GEQ T_EOF

// type-specific tokens
%token <sv_str> IDENTIFIER VALUE_STRING
%token <sv_int> VALUE_INT
%token <sv_float> VALUE_FLOAT
%token <sv_bool> VALUE_BOOL

// specify types for non-terminal symbol
%type <sv_node> stmt dbStmt ddl dml txnStmt setStmt
%type <sv_select_stmt> selectStmt subquery
%type <sv_field> field
%type <sv_fields> fieldList
%type <sv_type_len> type
%type <sv_comp_op> op
%type <sv_expr> expr
%type <sv_val> value
%type <sv_vals> valueList
%type <sv_str> tbName colName
%type <sv_strs> tableList colNameList
%type <sv_col> col
%type <sv_expr> selectItem
%type <sv_exprs> selectList selector
%type <sv_exprs> column_list
%type <sv_set_clause> setClause
%type <sv_set_clauses> setClauses
%type <sv_cond> condition
%type <sv_conds> whereClause optWhereClause
%type <sv_groupby> optGroupByClause
%type <sv_having_or> optHavingClause
%type <sv_having_and> havingAndClause
%type <sv_having_or> havingOrClause
%type <sv_orderby>  order_clause opt_order_clause
%type <sv_orderby_dir> opt_asc_desc
%type <sv_setKnobType> set_knob_type

%%
start:
        stmt ';'
    {
        parse_tree = $1;
        YYACCEPT;
    }
    |   HELP
    {
        parse_tree = std::make_shared<Help>();
        YYACCEPT;
    }
    |   EXIT
    {
        parse_tree = nullptr;
        YYACCEPT;
    }
    |   T_EOF
    {
        parse_tree = nullptr;
        YYACCEPT;
    }
    ;

stmt:
        dbStmt
    |   ddl
    |   dml
    |   txnStmt
    |   setStmt
    ;

txnStmt:
        TXN_BEGIN
    {
        $$ = std::make_shared<TxnBegin>();
    }
    |   TXN_COMMIT
    {
        $$ = std::make_shared<TxnCommit>();
    }
    |   TXN_ABORT
    {
        $$ = std::make_shared<TxnAbort>();
    }
    | TXN_ROLLBACK
    {
        $$ = std::make_shared<TxnRollback>();
    }
    ;

dbStmt:
        SHOW TABLES
    {
        $$ = std::make_shared<ShowTables>();
    }
    |   SHOW INDEX FROM tbName
    {
        $$ = std::make_shared<ShowIndex>($4);
    }
    ;

setStmt:
        SET set_knob_type '=' VALUE_BOOL
    {
        $$ = std::make_shared<SetStmt>($2, $4);
    }
    ;

ddl:
        CREATE TABLE tbName '(' fieldList ')'
    {
        $$ = std::make_shared<CreateTable>($3, $5);
    }
    |   DROP TABLE tbName
    {
        $$ = std::make_shared<DropTable>($3);
    }
    |   DESC tbName
    {
        $$ = std::make_shared<DescTable>($2);
    }
    |   CREATE INDEX tbName '(' colNameList ')'
    {
        $$ = std::make_shared<CreateIndex>($3, $5);
    }
    |   DROP INDEX tbName '(' colNameList ')'
    {
        $$ = std::make_shared<DropIndex>($3, $5);
    }
    ;

dml:
        INSERT INTO tbName VALUES '(' valueList ')'
    {
        $$ = std::make_shared<InsertStmt>($3, $6);
    }
    |   DELETE FROM tbName optWhereClause
    {
        $$ = std::make_shared<DeleteStmt>($3, $4);
    }
    |   UPDATE tbName SET setClauses optWhereClause
    {
        $$ = std::make_shared<UpdateStmt>($2, $4, $5);
    }
    |   selectStmt
    {
        $$ = $1;
    }
    ;

selectStmt:
        SELECT selector FROM tableList optWhereClause optGroupByClause optHavingClause opt_order_clause
    {
        $$ = std::make_shared<SelectStmt>($2, $4, $5, $6, $7, $8);
    }
    ;

fieldList:
        field
    {
        $$ = std::vector<std::shared_ptr<Field>>{$1};
    }
    |   fieldList ',' field
    {
        $$.push_back($3);
    }
    ;

colNameList:
        colName
    {
        $$ = std::vector<std::string>{$1};
    }
    | colNameList ',' colName
    {
        $$.push_back($3);
    }
    ;
column_list:
    col
    {
        $$ = std::vector<std::shared_ptr<Expr>>{$1};
    }
    | column_list ',' col
    {
        $$ = $1;
        $$.push_back($3);
    }
    ;
field:
        colName type
    {
        $$ = std::make_shared<ColDef>($1, $2);
    }
    ;

type:
        INT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_INT, sizeof(int));
    }
    |   CHAR '(' VALUE_INT ')'
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_STRING, $3);
    }
    |   FLOAT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_FLOAT, sizeof(float));
    }
    ;

valueList:
        value
    {
        $$ = std::vector<std::shared_ptr<Value>>{$1};
    }
    |   valueList ',' value
    {
        $$.push_back($3);
    }
    ;

value:
        VALUE_INT
    {
        $$ = std::make_shared<IntLit>($1);
    }
    |   VALUE_FLOAT
    {
        $$ = std::make_shared<FloatLit>($1);
    }
    |   VALUE_STRING
    {
        $$ = std::make_shared<StringLit>($1);
    }
    |   VALUE_BOOL
    {
        $$ = std::make_shared<BoolLit>($1);
    }
    ;

condition:
        expr op expr
    {
        $$ = std::make_shared<BinaryExpr>($1, $2, $3);
    }
    |   expr op subquery
    {
        $$ = std::make_shared<BinaryExpr>($1, $2, std::make_shared<SubqueryExpr>($3));
    }
    |   expr IN subquery
    {
        $$ = std::make_shared<BinaryExpr>($1, SV_OP_IN, std::make_shared<SubqueryExpr>($3));
    }
    |   expr IN '(' valueList ')'
    {
        $$ = std::make_shared<BinaryExpr>($1, SV_OP_IN, std::make_shared<ValueList>($4));
    }
    ;

subquery:
        '(' selectStmt ')'
    {
        $$ = $2;
    }
    ;

optWhereClause:
    /* empty */   { $$ = std::vector<std::shared_ptr<BinaryExpr>>{}; }
    |   WHERE whereClause { $$ = $2; }
    ;
optGroupByClause:
    /* empty */   { $$ = std::vector<std::shared_ptr<Expr>>{}; }
    | GROUP BY column_list   { $$ = $3; }
    ;

optHavingClause:
    /* empty */   { $$ = std::vector<std::vector<std::shared_ptr<BinaryExpr>>>{}; }
    | HAVING havingOrClause   { $$ = $2; }
    ;

havingOrClause:
    havingAndClause
    {
        $$ = std::vector<std::vector<std::shared_ptr<BinaryExpr>>>{$1};
    }
    | havingOrClause OR havingAndClause
    {
        $$ = $1;
        $$.push_back($3);
    }
    ;

havingAndClause:
    condition
    {
        $$ = std::vector<std::shared_ptr<BinaryExpr>>{$1};
    }
    | havingAndClause AND condition
    {
        $$ = $1;
        $$.push_back($3);
    }
    ;
whereClause:
        condition 
    {
        $$ = std::vector<std::shared_ptr<BinaryExpr>>{$1};
    }
    |   whereClause AND condition
    {
        $$.push_back($3);
    }
    ;

col:
        tbName '.' colName
    {
        $$ = std::make_shared<Col>($1, $3);
    }
    |   colName
    {
        $$ = std::make_shared<Col>("", $1);
    }
    ;

selectList:
    selectItem                    { $$ = std::vector<std::shared_ptr<Expr>>{$1}; }
    | selectList ',' selectItem   { $$ = $1; $$.push_back($3); }
    ;

selectItem:
    expr                  { $$ = $1; }
    | expr AS colName     { $$ = std::make_shared<AliasExpr>($1, $3); }
    ;
op:
        '='
    {
        $$ = SV_OP_EQ;
    }
    |   '<'
    {
        $$ = SV_OP_LT;
    }
    |   '>'
    {
        $$ = SV_OP_GT;
    }
    |   NEQ
    {
        $$ = SV_OP_NE;
    }
    |   LEQ
    {
        $$ = SV_OP_LE;
    }
    |   GEQ
    {
        $$ = SV_OP_GE;
    }
    ;

expr:
    value             { $$ = std::static_pointer_cast<Expr>($1); }
    | col             { $$ = std::static_pointer_cast<Expr>($1); }
    | SUM '(' col ')'   { $$ = std::make_shared<AggExpr>(AGG_SUM, $3); }
    | COUNT '(' col ')' { $$ = std::make_shared<AggExpr>(AGG_COUNT, $3); }
    | AVG '(' col ')'   { $$ = std::make_shared<AggExpr>(AGG_AVG, $3); }
    | MIN '(' col ')'   { $$ = std::make_shared<AggExpr>(AGG_MIN, $3); }
    | MAX '(' col ')'   { $$ = std::make_shared<AggExpr>(AGG_MAX, $3); }
    | COUNT '(' '*' ')' { $$ = std::make_shared<AggExpr>(AGG_COUNT_STAR, nullptr); }
    ;

setClauses:
        setClause
    {
        $$ = std::vector<std::shared_ptr<SetClause>>{$1};
    }
    |   setClauses ',' setClause
    {
        $$.push_back($3);
    }
    ;

setClause:
        colName '=' value
    {
        $$ = std::make_shared<SetClause>($1, $3);
    }
    ;

selector:
        '*'
    {
        $$ = {};
    }
    |   selectList
    ;

tableList:
        tbName
    {
        $$ = std::vector<std::string>{$1};
    }
    |   tableList ',' tbName
    {
        $$.push_back($3);
    }
    |   tableList JOIN tbName
    {
        $$.push_back($3);
    }
    ;

opt_order_clause:
    ORDER BY order_clause      
    { 
        $$ = $3; 
    }
    |   /* epsilon */ { /* ignore*/ }
    ;

order_clause:
      col  opt_asc_desc 
    { 
        $$ = std::make_shared<OrderBy>($1, $2);
    }
    ;   

opt_asc_desc:
    ASC          { $$ = OrderBy_ASC;     }
    |  DESC      { $$ = OrderBy_DESC;    }
    |       { $$ = OrderBy_DEFAULT; }
    ;    

set_knob_type:
    ENABLE_NESTLOOP { $$ = EnableNestLoop; }
    |   ENABLE_SORTMERGE { $$ = EnableSortMerge; }
    ;

tbName: IDENTIFIER;

colName: IDENTIFIER;
%%
