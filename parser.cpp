#include "parser.h"
#include <sstream>

//Выполняем синтаксический разбор блока program. Если во время разбора не обнаруживаем 
//никаких ошибок, то выводим последовательность команд стек-машины
void Parser::parse()
{
	program(); 
	if(!error_) {
		codegen_->flush();
	}
}

void Parser::program()
{
	mustBe(T_BEGIN);
	statementList();
	mustBe(T_END);
	codegen_->emit(STOP);
}

void Parser::statementList()
{
	//	  Если список операторов пуст, очередной лексемой будет одна из возможных "закрывающих скобок": END, OD, ELSE, FI.
	//	  В этом случае результатом разбора будет пустой блок (его список операторов равен null).
	//	  Если очередная лексема не входит в этот список, то ее мы считаем началом оператора и вызываем метод statement. 
	//    Признаком последнего оператора является отсутствие после оператора точки с запятой.
	if(see(T_END) || see(T_OD) || see(T_ELSE) || see(T_FI)) {
		return;
	}
	else {
		bool more = true;
		while(more) {
			statement();
			more = match(T_SEMICOLON);
		}
	}
}

void Parser::statement()
{
	Type type_statement = TYPE_INT;
	// Если встречаем переменную, то запоминаем ее адрес или добавляем новую если не встретили. 
	// Следующей лексемой должно быть присваивание. Затем идет блок expression, который возвращает значение на вершину стека.
	// Записываем это значение по адресу нашей переменной
	if(see(T_IDENTIFIER)) {
		string varName = scanner_->getStringValue();
		int varAddress = findOrAddVariable(varName);
		next();
		mustBe(T_ASSIGN);
		type_statement = expression();
		if (type_statement == TYPE_INT) {
			//Определяем тип новой переменной
			if (getType(varName) == TYPE_UNDEF) {
				findAndChangeType(varName, TYPE_INT);
			}
			else if (getType(varName) != TYPE_INT)
			{
				reportError("variable must be an integer");
				//Ошибка при попытке перезаписать переменную другого типа
			}
			codegen_->emit(STORE, varAddress);
		}
		else if (type_statement == TYPE_CMPLX)
		{	
			//Определяем тип новой переменной. Выделяем память для комплексных чисел
			if (getType(varName) == TYPE_UNDEF) {
				findAndChangeType(varName, TYPE_CMPLX);
				lastVar_++;
			}
			else if (getType(varName) != TYPE_CMPLX)
			{
				reportError("variable must be a complex"); 
				//Ошибка при попытке перезаписать переменную другого типа
			}
			
			codegen_->emit(STORE, varAddress);
			varAddress++;
			codegen_->emit(STORE, varAddress);
		}
		
	}
	// Если встретили IF, то затем должно следовать условие. На вершине стека лежит 1 или 0 в зависимости от выполнения условия.
	// Затем зарезервируем место для условного перехода JUMP_NO к блоку ELSE (переход в случае ложного условия). Адрес перехода
	// станет известным только после того, как будет сгенерирован код для блока THEN.
	else if(match(T_IF)) {
		relation();
		
		int jumpNoAddress = codegen_->reserve();

		mustBe(T_THEN);
		statementList();
		if(match(T_ELSE)) {
		//Если есть блок ELSE, то чтобы не выполнять его в случае выполнения THEN, 
		//зарезервируем место для команды JUMP в конец этого блока
			int jumpAddress = codegen_->reserve();
		//Заполним зарезервированное место после проверки условия инструкцией перехода в начало блока ELSE.
			codegen_->emitAt(jumpNoAddress, JUMP_NO, codegen_->getCurrentAddress());
			statementList();
		//Заполним второй адрес инструкцией перехода в конец условного блока ELSE.
			codegen_->emitAt(jumpAddress, JUMP, codegen_->getCurrentAddress());
		}
		else {
		//Если блок ELSE отсутствует, то в зарезервированный адрес после проверки условия будет записана
		//инструкция условного перехода в конец оператора IF...THEN
			codegen_->emitAt(jumpNoAddress, JUMP_NO, codegen_->getCurrentAddress());
		}

		mustBe(T_FI);
	}

	else if(match(T_WHILE)) {
		//запоминаем адрес начала проверки условия.
		int conditionAddress = codegen_->getCurrentAddress();
		relation();
		//резервируем место под инструкцию условного перехода для выхода из цикла.
		int jumpNoAddress = codegen_->reserve();
		mustBe(T_DO);
		statementList();
		mustBe(T_OD);
		//переходим по адресу проверки условия
		codegen_->emit(JUMP, conditionAddress);
		//заполняем зарезервированный адрес инструкцией условного перехода на следующий за циклом оператор.
		codegen_->emitAt(jumpNoAddress, JUMP_NO, codegen_->getCurrentAddress());
	}
	else if(match(T_WRITE)) {
		mustBe(T_LPAREN);
		Type type_write = expression();
		mustBe(T_RPAREN);
		if (type_write == TYPE_INT)
		{
			codegen_->emit(PRINT);
		}
		else if (type_write == TYPE_CMPLX)
		{
			codegen_->emit(PRINT);
			codegen_->emit(PRINT);
		}
		
	}
	else {
		reportError("statement expected.");
	}
}

Type Parser::expression()
{

	 /*
         Арифметическое выражение описывается следующими правилами: <expression> -> <term> | <term> + <term> | <term> - <term>
         При разборе сначала смотрим первый терм, затем анализируем очередной символ. Если это '+' или '-', 
		 удаляем его из потока и разбираем очередное слагаемое (вычитаемое). Повторяем проверку и разбор очередного 
		 терма, пока не встретим за термом символ, отличный от '+' и '-'
     */
	Type type_term = TYPE_INT;
	type_term = term();
	while(see(T_ADDOP)) {
		Arithmetic op = scanner_->getArithmeticValue();
		next();
		Type type_fstFactor = type_term;
		Type type_scndFactor = term();
		//приведение типов
		if (type_scndFactor != type_fstFactor) {
			type_term = TYPE_CMPLX;
			if (type_fstFactor == TYPE_INT) {
				codegen_->emit(STORE, lastVar_ + SHIFT);
				codegen_->emit(STORE, lastVar_ + SHIFT + 1);
				codegen_->emit(STORE, lastVar_ + SHIFT + 2);
				codegen_->emit(PUSH, 0);
				codegen_->emit(LOAD, lastVar_ + SHIFT + 2);
				codegen_->emit(LOAD, lastVar_ + SHIFT + 1);
				codegen_->emit(LOAD, lastVar_ + SHIFT);
			}
			else {
				codegen_->emit(STORE, lastVar_ + SHIFT);
				codegen_->emit(PUSH, 0);
				codegen_->emit(LOAD, lastVar_ + SHIFT);
			}
		}
		if (type_term == TYPE_INT) {

			if(op == A_PLUS) {
				codegen_->emit(ADD);
			}
			else {
				codegen_->emit(SUB);
			}
		}
		else if (type_term == TYPE_CMPLX)
		{
			codegen_->emit(STORE, lastVar_ + SHIFT);
			codegen_->emit(STORE, lastVar_ + SHIFT + 1);
			codegen_->emit(STORE, lastVar_ + SHIFT + 2);
			//codegen_->emit(STORE, lastVar_ + SHIFT + 3);
			//codegen_->emit(LOAD, lastVar_ + SHIFT + 3);
			codegen_->emit(LOAD, lastVar_ + SHIFT + 1);
			if (op == A_PLUS){
				codegen_->emit(ADD);
			}
			else {
				codegen_->emit(SUB);
			}
			codegen_->emit(LOAD, lastVar_ + SHIFT + 2);
			codegen_->emit(LOAD, lastVar_ + SHIFT);
			if (op == A_PLUS) {
				codegen_->emit(ADD);
			}
			else {
				codegen_->emit(SUB);
			}
		}
	}
	return type_term;
}

Type Parser::term()
{
	 /*  
		 Терм описывается следующими правилами: <expression> -> <factor> | <factor> + <factor> | <factor> - <factor>
         При разборе сначала смотрим первый множитель, затем анализируем очередной символ. Если это '*' или '/', 
		 удаляем его из потока и разбираем очередное слагаемое (вычитаемое). Повторяем проверку и разбор очередного 
		 множителя, пока не встретим за ним символ, отличный от '*' и '/' 
	*/
	Type type_term = factor();
	while(see(T_MULOP)) {
		Arithmetic op = scanner_->getArithmeticValue();
		next();
		Type type_fstFactor = type_term;
		Type type_scndFactor = factor();
		//приведение типов
		if (type_scndFactor != type_fstFactor) {
			type_term = TYPE_CMPLX;
			if (type_fstFactor == TYPE_INT){
				codegen_->emit(STORE, lastVar_ + SHIFT);
				codegen_->emit(STORE, lastVar_ + SHIFT + 1);
				codegen_->emit(STORE, lastVar_ + SHIFT + 2);
				codegen_->emit(PUSH, 0);
				codegen_->emit(LOAD, lastVar_ + SHIFT + 2);
				codegen_->emit(LOAD, lastVar_ + SHIFT + 1);
				codegen_->emit(LOAD, lastVar_ + SHIFT);
			}
			else {
				codegen_->emit(STORE, lastVar_ + SHIFT);
				codegen_->emit(PUSH, 0);
				codegen_->emit(LOAD, lastVar_ + SHIFT);
			}
		}
		if (type_term == TYPE_INT)
		{
			if (op == A_MULTIPLY) {
				codegen_->emit(MULT);
			}
			else {
				codegen_->emit(DIV);
			}
		}
		else if (type_term == TYPE_CMPLX)
		{
			codegen_->emit(STORE, lastVar_ + SHIFT);
			codegen_->emit(STORE, lastVar_ + SHIFT + 1);
			codegen_->emit(STORE, lastVar_ + SHIFT + 2);
			codegen_->emit(STORE, lastVar_ + SHIFT + 3);
			codegen_->emit(LOAD, lastVar_ + SHIFT + 3);
			codegen_->emit(LOAD, lastVar_ + SHIFT);
			codegen_->emit(MULT);
			codegen_->emit(LOAD, lastVar_ + SHIFT + 2);
			codegen_->emit(LOAD, lastVar_ + SHIFT + 1);
			codegen_->emit(MULT);
			if (op == A_MULTIPLY) {
				codegen_->emit(ADD);
			}
			else {
				codegen_->emit(SUB);
				codegen_->emit(LOAD, lastVar_ + SHIFT);
				codegen_->emit(LOAD, lastVar_ + SHIFT);
				codegen_->emit(MULT);
				codegen_->emit(LOAD, lastVar_ + SHIFT + 1);
				codegen_->emit(LOAD, lastVar_ + SHIFT + 1);
				codegen_->emit(MULT);
				codegen_->emit(ADD);
				codegen_->emit(STORE, lastVar_ + SHIFT + 4);
				codegen_->emit(LOAD, lastVar_ + SHIFT + 4);
				codegen_->emit(DIV);
			}
			codegen_->emit(LOAD, lastVar_ + SHIFT + 2);
			codegen_->emit(LOAD, lastVar_ + SHIFT);
			codegen_->emit(MULT);
			codegen_->emit(LOAD, lastVar_ + SHIFT + 3);
			codegen_->emit(LOAD, lastVar_ + SHIFT + 1);
			codegen_->emit(MULT);
			if (op == A_MULTIPLY) {
				codegen_->emit(SUB);
			}
			else {
				codegen_->emit(ADD);
				codegen_->emit(LOAD, lastVar_ + SHIFT + 4);
				codegen_->emit(DIV);
			}
		}
	}
	return type_term;
}

Type Parser::factor()
{
	Type type_factor = TYPE_INT;
	/*
		Множитель описывается следующими правилами:
		<factor> -> number | identifier | -<factor> | (<expression>) | READ
	*/
	if(see(T_NUMBER)) {
		int value = scanner_->getIntValue();
		next();
		codegen_->emit(PUSH, value);
		//Если встретили число, то преобразуем его в целое и записываем на вершину стека
	}
	else if (see(T_COMPLEX)) {
		int value = scanner_->getIntValue();
		int cmplx_value = scanner_->getCmplxValue();
		next();
		codegen_->emit(PUSH, cmplx_value);
		codegen_->emit(PUSH, value);
		type_factor = TYPE_CMPLX;
	}
	else if(see(T_IDENTIFIER)) {
		int varAddress = findOrAddVariable(scanner_->getStringValue());
		Type varType = getType(scanner_->getStringValue());
		next();
		if (varType == TYPE_INT)
		{
			codegen_->emit(LOAD, varAddress);
		}
		if (varType == TYPE_CMPLX)
		{
			codegen_->emit(LOAD, ++varAddress);
			codegen_->emit(LOAD, --varAddress);
		}
		type_factor = varType; // Возвращает тип переменной
		//Если встретили переменную, то выгружаем значение, лежащее по ее адресу, на вершину стека 
	}
	else if(see(T_ADDOP) && scanner_->getArithmeticValue() == A_MINUS) {
		next();
		type_factor = factor();
		if (type_factor == TYPE_INT) {
			codegen_->emit(INVERT);
		}
		else {
			codegen_->emit(INVERT);
			codegen_->emit(STORE, lastVar_ + SHIFT);
			codegen_->emit(INVERT);
			codegen_->emit(LOAD, lastVar_ + SHIFT);
		}
		//Если встретили знак "-", и за ним <factor> то инвертируем значение, лежащее на вершине стека
	}
	else if(match(T_LPAREN)) {
		type_factor = expression();
		mustBe(T_RPAREN);
		//Если встретили открывающую скобку, тогда следом может идти любое арифметическое выражение и обязательно
		//закрывающая скобка.
	}
	else if(match(T_READ)) {
		if (match(T_LPAREN)) {
			mustBe(T_TYPE);
			if (scanner_->getTypeValue() == "int") {
				codegen_->emit(INPUT);
				type_factor = TYPE_INT;
			}
			else if (scanner_->getTypeValue() == "complex") {
				codegen_->emit(INPUT);
				codegen_->emit(STORE, lastVar_ + SHIFT);
				codegen_->emit(INPUT);
				codegen_->emit(LOAD, lastVar_ + SHIFT);
				type_factor = TYPE_CMPLX;
			}
			else {
				reportError("Uknown type using");
			}
			mustBe(T_RPAREN);
		}
		else {
			codegen_->emit(INPUT);
		}
		//Если встретили зарезервированное слово READ, то записываем на вершину стека идет запись со стандартного ввода
	}
	else {
		reportError("expression expected.");
	}
	return type_factor;
}

void Parser::relation()
{
	//Условие сравнивает два выражения по какому-либо из знаков. Каждый знак имеет свой номер. В зависимости от 
	//результата сравнения на вершине стека окажется 0 или 1.
	Type fstExpression, scndExpession;

	fstExpression = expression();
	if(see(T_CMP)) {
		Cmp cmp = scanner_->getCmpValue();
		next();
		scndExpession = expression();
		if (fstExpression == TYPE_INT && scndExpession == TYPE_INT) {
			switch (cmp) {
				//для знака "=" - номер 0
			case C_EQ:
				codegen_->emit(COMPARE, 0);
				break;
				//для знака "!=" - номер 1
			case C_NE:
				codegen_->emit(COMPARE, 1);
				break;
				//для знака "<" - номер 2
			case C_LT:
				codegen_->emit(COMPARE, 2);
				break;
				//для знака ">" - номер 3
			case C_GT:
				codegen_->emit(COMPARE, 3);
				break;
				//для знака "<=" - номер 4
			case C_LE:
				codegen_->emit(COMPARE, 4);
				break;
				//для знака ">=" - номер 5
			case C_GE:
				codegen_->emit(COMPARE, 5);
				break;
			};
		}
		else if (fstExpression == TYPE_CMPLX && scndExpession == TYPE_CMPLX) {
			Cmp cmp = scanner_->getCmpValue();
			if (cmp == C_EQ)	{
				codegen_->emit(STORE, lastVar_ + SHIFT);
				codegen_->emit(STORE, lastVar_ + SHIFT + 1);
				codegen_->emit(STORE, lastVar_ + SHIFT + 2);
				codegen_->emit(LOAD, lastVar_ + SHIFT + 1);
				codegen_->emit(COMPARE, 0);
				codegen_->emit(LOAD, lastVar_ + SHIFT);
				codegen_->emit(LOAD, lastVar_ + SHIFT + 2);
				codegen_->emit(COMPARE, 0);
				codegen_->emit(MULT);
			}
			else if (cmp == C_NE) {
				codegen_->emit(STORE, lastVar_ + SHIFT);
				codegen_->emit(STORE, lastVar_ + SHIFT + 1);
				codegen_->emit(STORE, lastVar_ + SHIFT + 2);
				codegen_->emit(LOAD, lastVar_ + SHIFT + 1);
				codegen_->emit(COMPARE, 1);
				codegen_->emit(LOAD, lastVar_ + SHIFT);
				codegen_->emit(LOAD, lastVar_ + SHIFT + 2);
				codegen_->emit(COMPARE, 1);
				codegen_->emit(ADD);
			}
			else {
				reportError("comparison operator is not defined for complex variables.");
			}
		}
		else {
			reportError("comparison operator is not defined for different type variables.");
		}
	}
	else {
		reportError("comparison operator expected.");
	}
}

int Parser::findOrAddVariable(const string& var, Type type)
{
	VarTable::iterator it = variables_.find(var);
	if(it == variables_.end()) {
		variables_[var] = Variable (type, lastVar_);
		return lastVar_++;
	}
	else {

		return it->second.second;
	}
}

void Parser::findAndChangeType(const string& var, Type type)
{
	VarTable::iterator it = variables_.find(var);
	if (it != variables_.end()) {
		variables_[var].first = type;
	}
}
Type Parser::getType(const string& var)
{
	VarTable::iterator it = variables_.find(var);
	if (it != variables_.end()) {
		return variables_[var].first;
	}
	else return TYPE_INT;
}
void Parser::mustBe(Token t)
{
	if(!match(t)) {
		error_ = true;

		// Подготовим сообщение об ошибке
		std::ostringstream msg;
		msg << tokenToString(scanner_->token()) << " found while " << tokenToString(t) << " expected.";
		reportError(msg.str());

		// Попытка восстановления после ошибки.
		recover(t);
	}
}

void Parser::recover(Token t)
{
	while(!see(t) && !see(T_EOF)) {
		next();
	}

	if(see(t)) {
		next();
	}
}
